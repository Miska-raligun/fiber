#include "fiber.h"

static bool debug = true;

namespace sylar {

// 当前线程上的协程控制信息

// 正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 协程计数器
static std::atomic<uint64_t> s_fiber_id{0};
// 协程id
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber *f)
{
	t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()
{
	if(t_fiber)
	{	
		return t_fiber->shared_from_this();
	}

	std::shared_ptr<Fiber> main_fiber(new Fiber());
	t_thread_fiber = main_fiber;
	t_scheduler_fiber = main_fiber.get(); // 除非主动设置 主协程默认为调度协程
	
	assert(t_fiber == main_fiber.get());
	return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber* f)
{
	t_scheduler_fiber = f;
}

uint64_t Fiber::GetFiberId()
{
	if(t_fiber)
	{
		return t_fiber->getId();
	}
	return (uint64_t)-1;
}

Fiber::Fiber()
{
	SetThis(this);
	m_state = RUNNING;
	
	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber() failed\n";
		pthread_exit(NULL);
	}
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

//param1：协程函数;param2:栈空间;param3:是否让出执行权给调度协程
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler)
{
	m_state = READY;

	// 分配协程栈空间,如果为0，则设置为默认大小128k
	m_stacksize = stacksize ? stacksize : 128000;
	m_stack = malloc(m_stacksize);

	//getcontext(uncontext_t *ucp):获取协程上下文，成功返回0，失败返回-1
	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		pthread_exit(NULL);
	}
	
	//当前上下文结束后，下一个激活的上下文对象的指针
	m_ctx.uc_link = nullptr;
	//上下文使用的栈空间指针
	m_ctx.uc_stack.ss_sp = m_stack;
	//栈空间大小
	m_ctx.uc_stack.ss_size = m_stacksize;

	//makecontext()
	//param1:上下文指针;param2:目标函数;param3:运行参数
	//修改上下文指针，使其与一个函数绑定，可以指定func运行的参数
	//可以指定uc_link,表示运行结束后指向的上下文,如果不指定，则函数结束时必须调用setcontext或swapcontext重新指定一个上下文
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

//释放栈空间
Fiber::~Fiber()
{
	s_fiber_count --;
	if(m_stack)
	{
		free(m_stack);
	}
	if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}

//重置协程状态及入口函数，复用栈空间
void Fiber::reset(std::function<void()> cb)
{
	//当前协程状态为终止且栈空间不为空时才能reset
	assert(m_stack != nullptr&&m_state == TERM);

	//更改协程状态及入口函数
	m_state = READY;
	m_cb = cb;

	//获取上下文
	if(getcontext(&m_ctx))
	{
		std::cerr << "reset() failed\n";
		pthread_exit(NULL);
	}

	//更新上下文状态及栈指针
	m_ctx.uc_link = nullptr;
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

//将当前协程切换到运行状态
void Fiber::resume()
{
	//当前协程就绪时才能切换
	assert(m_state==READY);
	
	m_state = RUNNING;

	//由调度协程调度还是由主协程调度
	if(m_runInScheduler)
	{
		SetThis(this);
		//交换调度协程的上下文和当前协程上下文
		//上下文切换至当前协程上下文
		if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	else
	{
		SetThis(this);
		//交换主协程上下文和当前协程上下文
		if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}
}

//当前协程让出执行权
//交换当前协程与上次resume时退到后台的协程
void Fiber::yield()
{
	assert(m_state==RUNNING || m_state==TERM);

	if(m_state!=TERM)
	{
		m_state = READY;
	}

	//交换当前协程与调度携程
	if(m_runInScheduler)
	{
		SetThis(t_scheduler_fiber);
		if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
		{
			std::cerr << "yield() to to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	//交换主协程与调度协程
	else
	{
		SetThis(t_thread_fiber.get());
		if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
		{
			std::cerr << "yield() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}	
}

void Fiber::MainFunc()
{
	std::shared_ptr<Fiber> curr = GetThis();
	assert(curr!=nullptr);

	//执行协程函数
	curr->m_cb(); 

	//运行完毕,更新状态
	curr->m_cb = nullptr;
	curr->m_state = TERM;

	// 运行完毕 -> 让出执行权
	auto raw_ptr = curr.get();
	curr.reset(); 
	raw_ptr->yield(); 
}

}