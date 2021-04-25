/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>
#include <iostream>
#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

/*
 1. 每当启动（resume）一个协程时，就将它的协程控制块 stCoRoutine_t 结构指针保存在 pCallStack 的“栈顶”，
 2. 然后“栈指针” iCallStackSize 加 1，最后切换 context 到待启动协程运行。当协程要让出（yield）CPU 时，
 3. 就将它的 stCoRoutine_t从pCallStack 弹出，“栈指针” iCallStackSize 减 1，
 4. 然后切换 context 到当前栈顶的协程（原来被挂起的调用者）恢复执
 */
// stCoRoutineEnv_t结构一个线程只有一个
struct stCoRoutineEnv_t
{
    // 如果将协程看成一种特殊的函数，那么这个 pCallStack 就时保存这些函数的调用链的栈。
    // 非对称协程最大特点就是协程间存在明确的调用关系；甚至在有些文献中，启动协程被称作 call，
    // 挂起协程叫 return。非对称协程机制下的被调协程只能返回到调用者协程，这种调用关系不能乱，
    // 因此必须将调用链保存下来
    stCoRoutine_t *pCallStack[ 128 ];
    int iCallStackSize; // 上面那个调用栈的栈顶指针
    // epoll的一个封装结构
    stCoEpoll_t *pEpoll;

    // for copy stack log lastco and nextco
    // 对上次切换挂起的协程和嵌套调用的协程栈的拷贝,为了减少共享栈上数据的拷贝
    // 在不使用共享栈模式时 pending_co 和 ocupy_co 都是空指针
    // pengding是目前占用共享栈的协程
    // 想想看,如果不加的话,我们需要O(N)的时间复杂度分清楚Callback中current上一个共享栈的协程实体(可能共享栈与默认模式混合)
    stCoRoutine_t* pending_co;
    // 与pending在同一个共享栈上的上一个协程
    stCoRoutine_t* occupy_co;
};
/*
pCallStack结构是一个非常重要的结构，这个名字起的非常有意思，很贴切，因为这就是一个调用栈，它存储着协程的调用栈，举个例子，主协程A调用协程B，协程B的函数中又调用协程C，这个时候pCallStack中存储的数据就是[A，B，C]，拿我们前面举过的生产者消费者模型距离，把生产者当做B，消费者当做C，主协程当做A，pCallStack的结构就在[A，B]，[A，C]，间切换。简单来说每一项前面存储着调用这个协程的协程，最少有一个元素，即主协程。
pEpoll，一个封装的epoll。
剩下两个结构与共享栈相关，存储着与当前运行线程使用同一个栈的线程，因为共享栈可能有多个，参见sharestack结构中栈结构其实是个数组。个人认为加上的原因就是检索更快。
 */
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}


static unsigned long long GetTickMS()
{

	struct timeval now = { 0 };
	gettimeofday( &now,NULL );
	unsigned long long u = now.tv_sec;
	u *= 1000;
	u += now.tv_usec / 1000;
	return u;
}


template <class T,class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );

	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}

template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink )
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}

template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}

/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
	stack_mem->occupy_co= NULL;
	stack_mem->stack_size = stack_size;
	stack_mem->stack_buffer = (char*)malloc(stack_size);
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
	return stack_mem;
}

stShareStack_t* co_alloc_sharestack(int count, int stack_size)
{
	stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
	share_stack->alloc_idx = 0;
	share_stack->stack_size = stack_size;

	//alloc stack array
	share_stack->count = count;
	stStackMem_t** stack_array = (stStackMem_t**)calloc(count, sizeof(stStackMem_t*));
	for (int i = 0; i < count; i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size);
	}
	share_stack->stack_array = stack_array;
	return share_stack;
}


static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
	if (!share_stack)
	{
		return NULL;
	}
	int idx = share_stack->alloc_idx % share_stack->count;
	share_stack->alloc_idx++;

	return share_stack->stack_array[idx];
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;
struct stCoEpoll_t
{
    int iEpollFd; 								// epollfd
    static const int _EPOLL_SIZE = 1024 * 10;   // 一次 epoll_wait 最多返回的就绪事件个数

    struct stTimeout_t *pTimeout; 				// 单轮时间轮

    struct stTimeoutItemLink_t *pstTimeoutList;	// 链表用于临时存放超时事件的item

    struct stTimeoutItemLink_t *pstActiveList;	// 该链表用于存放epoll_wait得到的就绪事件和定时器超时事件

    // 对 epoll_wait() 第二个参数的封装，即一次 epoll_wait 得到的结果集
    co_epoll_res *result;

};

typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);
struct stTimeoutItem_t
{

	enum
	{
		eMaxTimeout = 40 * 1000 //40s
	};
	stTimeoutItem_t *pPrev;
	stTimeoutItem_t *pNext;
	stTimeoutItemLink_t *pLink;

	unsigned long long ullExpireTime;

	OnPreparePfn_t pfnPrepare;
	OnProcessPfn_t pfnProcess;

	void *pArg; // routine 
	bool bTimeout;
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;

};
/*
* 毫秒级的超时管理器
* 使用时间轮实现
* 但是是有限制的，最长超时时间不可以超过iItemSize毫秒
*/
struct stTimeout_t
{
    /*
       时间轮
       超时事件数组，总长度为iItemSize,每一项代表1毫秒，为一个链表，代表这个时间所超时的事件。
       这个数组在使用的过程中，会使用取模的方式，把它当做一个循环数组来使用，虽然并不是用循环链表来实现的
    */
    stTimeoutItemLink_t *pItems;
    int iItemSize;		// 数组长度

    unsigned long long ullStart; // 时间轮第一次使用的时间
    long long llStartIdx;	// 目前正在使用的下标
};
/*
极其疑惑，就一个链表，它什么就叫时间轮了？
 注释中已经很清楚了，在这里的时候我想到了以前对时间轮的思考，这里到底是单轮时间轮效率高，还是多轮时间轮效率高呢？
 我想这个问题没有什么意义，因为对于时间轮的选择取决于事件的超时时间。不给出场景讨论效率就是耍流氓。
 一般来说单轮时间轮复杂度降低的时候超时时间大于时间轮长度的时候需要取余放入，导致每次从时间轮取出的时候都会有一些无效的遍历，libco在超时时间大于时间轮长度的时候就直接拒绝了。
 而多轮时间轮因为其特性很难出现超时时间大于时间轮长度，所有就没有了无效遍历，但是需要一些拷贝。
*/

stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems );
	free ( apTimeout );
}
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;

	if( diff >= (unsigned long long)apTimeout->iItemSize )
	{
		diff = apTimeout->iItemSize - 1;
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);

		//return __LINE__;
	}
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );

	return 0;
}
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	if( allNow < apTimeout->ullStart )
	{
		return ;
	}
	int cnt = allNow - apTimeout->ullStart + 1;
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );
	}
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;


}
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
    std::cout<<"static int CoRoutineFunc( stCoRoutine_t *co,void * )"<<std::endl;
	if( co->pfn )
	{
		co->pfn( co->arg );  // 这里进入到了void* Consumer(void* args)
    }
	co->cEnd = 1;

	stCoRoutineEnv_t *env = co->env;

	co_yield_env( env );

	return 0;
}



/**
 * @env  环境变量
 * @attr 协程信息
 * @pfn  函数指针
 * @arg  函数参数
*/
struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env, const stCoRoutineAttr_t* attr,
                                     pfn_co_routine_t pfn,void *arg )
{

    stCoRoutineAttr_t at;
    // 如果指定了attr的话就执行拷贝
    if( attr )
    {
        memcpy( &at,attr,sizeof(at) );
    }
    // stack_size 有效区间为[0, 1024 * 1024 * 8]
    if( at.stack_size <= 0 )
    {
        at.stack_size = 128 * 1024;
    }
    else if( at.stack_size > 1024 * 1024 * 8 )
    {
        at.stack_size = 1024 * 1024 * 8;
    }

    // 4KB对齐,也就是说如果对stacksize取余不为零的时候对齐为4KB
    // 例如本来5KB,经过了这里就变为8KB了
    if( at.stack_size & 0xFFF )
    {
        at.stack_size &= ~0xFFF;
        at.stack_size += 0x1000;
    }

    // 为协程分配空间
    stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );

    memset( lp,0,(long)(sizeof(stCoRoutine_t)));


    lp->env = env;
    lp->pfn = pfn;
    lp->arg = arg;

    stStackMem_t* stack_mem = NULL;
    if( at.share_stack ) // 共享栈模式 栈需要自己指定
    {
        stack_mem = co_get_stackmem( at.share_stack);
        at.stack_size = at.share_stack->stack_size;
    }
    else // 每个协程有一个私有的栈
    {
        stack_mem = co_alloc_stackmem(at.stack_size);
    }
    lp->stack_mem = stack_mem;

    lp->ctx.ss_sp = stack_mem->stack_buffer; // 这个协程栈的基址
    lp->ctx.ss_size = at.stack_size;// 未使用大小,与前者相加为esp指针,见coctx_make解释

    lp->cStart = 0;
    lp->cEnd = 0;
    lp->cIsMain = 0;
    lp->cEnableSysHook = 0;
    lp->cIsShareStack = at.share_stack != NULL;

    lp->save_size = 0;
    lp->save_buffer = NULL;

    return lp;
}
/*
这里有一点需要说，就是ss_size其实是未使用的大小，为什么要记录未使用大小呢？
 我们思考一个问题，这个栈其实是要把基址付给寄存器的，而系统栈中指针由高地址向低地址移动，而我们分配的堆内存实际上低地址是起始地址，
 这里是把从线程分配的堆内存当做协程的栈，所以esp其实是指向这片堆地址的最末尾的，所以记录未使用大小，使得基址加上未使用大小就是esp。
 简单用简笔画描述一下：
|------------|
|     esp    |
|------------|
|   ss_size  |
|------------|
|stack_buffer|
|------------|
*/

int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	if( !co_get_curr_thread_env() ) 
	{
		co_init_curr_thread_env();
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(), attr, pfn,arg );
	*ppco = co;
	return 0;
}
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer);
        free(co->stack_mem);
    }   
    //walkerdu fix at 2018-01-20
    //存在内存泄漏
    else 
    {
        if(co->save_buffer)
            free(co->save_buffer);

        if(co->stack_mem->occupy_co == co)
            co->stack_mem->occupy_co = NULL;
    }

    free( co );
}
void co_release( stCoRoutine_t *co )
{
    co_free( co );
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);

void co_resume( stCoRoutine_t *co )
{
    // stCoRoutine_t结构需要我们在我们的代码中自行调用co_release或者co_free
    stCoRoutineEnv_t *env = co->env;

    // 获取当前正在进行的协程主体coctx_make
    stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];
    if(!co->cStart ) //
    {
        coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 ); //保存上一个协程的上下文
        co->cStart = 1;
    }
    // 把此次执行的协程控制块放入调用栈中
    env->pCallStack[ env->iCallStackSize++ ] = co;
    // co_swap() 内部已经切换了 CPU 执行上下文
    co_swap( lpCurrRoutine, co );

}
/*
我们可以看到首先从线程唯一的env中的调用栈中拿到了调用此协程的协程实体，也就是现在正在运行的这个协程的实体。我们首先要明确一个问题，就是co_resume并不是只调用一次，伴随着协程主动让出执行权，它总要被再次执行，靠的就是这个co_resume函数，无非第一次调用的时候需要初始化寄存器信息，后面不用罢了。

co->cStart标记了这个协程是否是第一次执行co_resume，关于coctx_make，我打算用一篇单独的文章讲解，因为确实比较麻烦。我们先来看看其他的逻辑，现在暂且知道coctx_make所做的事情就是为coctx_swap函数的正确执行去初始化co->ctx中寄存器信息。

然后就是把此次执行的协程放入调用栈中并自增iCallStackSize，iCallStackSize自增后是调用栈中目前有的协程数量。
stStackMem_t && stShareStack_t

接下来就是核心函数co_swap，它执行了协程的切换，并做了一些其他的工作。不过在co_swap之前我希望先说说libco的两种协程栈的策略，一种是一个协程分配一个栈，这也是默认的配置，不过缺点十分明显，因为默认大小为128KB，如果1024个协程就是128MB，1024*1024个协程就是128GB，好像和协程“千万连接”相差甚远。且这些空间中显然有很多的空隙，可能很多协程只用了1KB不到，这显然是一种极大的浪费。所以还有另一种策略，即共享栈。看似高大上，实则没什么意思，还记得我此一次看到这个名词的时候非常疑惑，想了很久如何才能高效的实现一个共享栈，思考无果后查阅libco源码，出乎意料，libco的实现并不高效，但是能跑，且避免了默认配置的情况。答案就是在进行协程切换的时候把已经使用的内存进行拷贝。这样一个线程所有的协程在运行时使用的确实是同一个栈，也就是我们所说的共享栈了。
*/


// walkerdu 2018-01-14                                                                              
// 用于reset超时无法重复使用的协程                                                                  
void co_reset(stCoRoutine_t * co)
{
    if(!co->cStart || co->cIsMain)
        return;

    co->cStart = 0;
    co->cEnd = 0;

    // 如果当前协程有共享栈被切出的buff，要进行释放
    if(co->save_buffer)
    {
        free(co->save_buffer);
        co->save_buffer = NULL;
        co->save_size = 0;
    }

    // 如果共享栈被当前协程占用，要释放占用标志，否则被切换，会执行save_stack_buffer()
    if(co->stack_mem->occupy_co == co)
        co->stack_mem->occupy_co = NULL;
}

void co_yield_env( stCoRoutineEnv_t *env )
{
	
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ];
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ];

	env->iCallStackSize--;

	co_swap( curr, last);
}

void co_yield_ct()
{

	co_yield_env( co_get_curr_thread_env() );
}
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}

void save_stack_buffer(stCoRoutine_t* occupy_co)
{
	///copy out
	stStackMem_t* stack_mem = occupy_co->stack_mem;
	int len = stack_mem->stack_bp - occupy_co->stack_sp;

	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
	}

	occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
	occupy_co->save_size = len;

	memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

// 当前准备让出 CPU 的协程叫做 current 协程，把即将调入执行的叫做 pending 协程
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
    stCoRoutineEnv_t* env = co_get_curr_thread_env();

    // get curr stack sp
    // 获取esp指针,
    char c;
    curr->stack_sp= &c;

    if (!pending_co->cIsShareStack)
    {
        env->pending_co = NULL;
        env->occupy_co = NULL;
    }
    else // 如果采用了共享栈
    {
        env->pending_co = pending_co;
        // get last occupy co on the same stack mem
        // 获取pending使用的栈空间的执行协程
        stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
        // 也就是当前正在执行的进程
        // set pending co to occupy thest stack mem
        // 将该共享栈的占用者改为pending_co
        pending_co->stack_mem->occupy_co = pending_co;

        env->occupy_co = occupy_co;
        if (occupy_co && occupy_co != pending_co)
        {
            // 如果上一个使用协程不为空,则需要把它的栈内容保存起来
            save_stack_buffer(occupy_co);
        }
    }

    //swap context 这个函数执行完, 就切入下一个协程了
    coctx_swap(&(curr->ctx),&(pending_co->ctx) );
    /*
    coctx_swap是libco的核心，即一段汇编代码，功能为把传入的两个把当前寄存器的值存入curr中，把pending中的值存入寄存器中，也就是我们所说的上下文切换。

    我们脑子要清楚一件事情。就是coctx_swap执行完以后，CPU就跑去执行pendding中的代码了，
    也就是说执行完执行coctx_swap的这条语句后，下一条要执行的语句不是stCoRoutineEnv_t* curr_env = co_get_curr_thread_env()，而是pedding中的语句。这一点要尤其注意。

    那么什么时候执行coctx_swap这条语句之后的语句呢？就是在协程被其他地方执行co_resume了以后才会继续执行这里。
    后面就简单啦，切换出去的时候要把栈内容拷贝下来，切换回来当然又要拷贝到栈里面啦。
    */
    //stack buffer may be overwrite, so get again;
    stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
    stCoRoutine_t* update_occupy_co =  curr_env->occupy_co;
    stCoRoutine_t* update_pending_co = curr_env->pending_co;

    if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
    {
        //resume stack buffer
        if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
        {
            // 如果是一个协程执行到一半然后被切换出去然后又切换回来,这个时候需要恢复栈空间
            memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
        }
    }
}
/*
首先我们可以看到令人疑惑的一句代码：
char c;
curr->stack_sp= &c;
说实话，在第一次看到的时候，实在是疑惑至极，这玩意在干嘛，但仔细想，我们上面说了当协程选择共享栈的时候需要在协程切换是拷贝栈上已使用的内存。问题来了，拷贝哪一部分呢？
|------------|
|	 used    |
|------------|
|     esp    |
|------------|
|   ss_size  |
|------------|
|stack_buffer|
|------------|
答案就是usd部分，首先栈底我们可以很容易的得到。就是栈基址加上栈大小，但是esp怎么获取呢？再写一段汇编代码？可以，但没必要。
 libco采用了一个极其巧妙的方法，个人认为这是libco最精妙的几段代码之一。就是直接用一个char类型的指针放在函数头，获取esp。
 这样我们就得到了需要保存的数据范围了。

然后就是一段更新env中pengding与occupy_co的代码，最后执行共享栈中栈的保存。


*/


//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds;
	nfds_t nfds; // typedef unsigned long int nfds_t;

	stPollItem_t *pPollItems;

	int iAllEventDetach;

	int iEpollFd;

	int iRaiseCnt;


};
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;
	stPoll_t *pPoll;

	struct epoll_event stEvent;
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}

static __thread stCoRoutineEnv_t* gCoEnvPerThread = NULL;   //curr_thread_env
// 这个函数其实也就是在每个线程的第一个协程被创建的时候去初始化gCoEnvPerThread
void co_init_curr_thread_env()
{
    gCoEnvPerThread = (stCoRoutineEnv_t*)calloc( 1, sizeof(stCoRoutineEnv_t) );
    stCoRoutineEnv_t *env = gCoEnvPerThread;

    env->iCallStackSize = 0;	// 修改"调用栈"顶指针
    struct stCoRoutine_t *self = co_create_env( env, NULL, NULL,NULL );  // 主协程
    self->cIsMain = 1;			// 一个线程调用这个函数的肯定是主协程喽

    env->pending_co = NULL;
    env->occupy_co = NULL;

    coctx_init( &self->ctx ); // 能跑到这里一定是main,所以清空上下文

    env->pCallStack[ env->iCallStackSize++ ] = self; // 放入线程独有环境中

    stCoEpoll_t *ev = AllocEpoll();
    SetEpoll( env,ev );
}
/*
首先为gCoEnvPerThread分配一份内存，这本没什么说的，但是这也显示出libco其实整体是一个偏C的写法。
 硬要说它是C++的话。可能就是用了一些STL吧，还有一点，就是为什么不使用RAII去管理内存，而是使用原始的手动管理内存？
 我的想法是为了提升效率，库本身并没有什么预期之外操作会出现，所以不存在运行到一半throw了（当然整个libco也没有throw），
 手动管理内存完全是可以的，只不过比较麻烦罢了，不过确实省去了智能指针的开销。

后面调用了co_create_env创建了一个stCoRoutine_t类型的结构，我们前面说过stCoRoutine_t其实是一个协程的实体，存储着协程的所有信息，
 这里创建的了一个协程是为什么呢？仔细一想再结合着后面的IsMain就非常明显了，这个结构是主协程，
 因为co_init_curr_thread_env在一个线程内只会被调用一次，那么调用这个函数的线程理所当然就是主协程喽。
 co_create_env我们后面再说。

创建协程的下面四句其实都是一些内部成员的初始化，第五句其实是有些意思的，把self付给了pCallStack，并自增iCallStackSize，
 我们前面说过pCallStack其实是一个调用栈的结构，那么这个调用栈的第一个肯定是主协程，
 第0个元素是self，然后iCallStackSize增为1，等待主协程调用其他协程的时候放入调用栈。
*/

stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return gCoEnvPerThread;
}

void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap;
	lp->pSelf->revents = EpollEvent2Poll( e.events );


	stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++;

	if( !pPoll->iAllEventDetach )
	{
		pPoll->iAllEventDetach = 1;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );

		AddTail( active,pPoll );

	}
}


void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg )
{
	if( !ctx->result )
	{
		ctx->result =  co_epoll_res_alloc( stCoEpoll_t::_EPOLL_SIZE );
	}
	co_epoll_res *result = ctx->result;


	for(;;)
	{
		int ret = co_epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );

		stTimeoutItemLink_t *active = (ctx->pstActiveList);
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);

		memset( timeout,0,sizeof(stTimeoutItemLink_t) );

		for(int i=0;i<ret;i++)
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;
			if( item->pfnPrepare )
			{
				item->pfnPrepare( item,result->events[i],active );
			}
			else
			{
				AddTail( active,item );
			}
		}


		unsigned long long now = GetTickMS();
		TakeAllTimeout( ctx->pTimeout,now,timeout );

		stTimeoutItem_t *lp = timeout->head;
		while( lp )
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}

		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout );

		lp = active->head;
		while( lp )
		{

			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
            if (lp->bTimeout && now < lp->ullExpireTime) 
			{
				int ret = AddTimeout(ctx->pTimeout, lp, now);
				if (!ret) 
				{
					lp->bTimeout = false;
					lp = active->head;
					continue;
				}
			}
			if( lp->pfnProcess )
			{
				lp->pfnProcess( lp );
			}

			lp = active->head;
		}
		if( pfn )
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}

	}
}
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}


stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );

	ctx->iEpollFd = co_epoll_create( stCoEpoll_t::_EPOLL_SIZE );
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


	return ctx;
}

void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
		co_epoll_res_free( ctx->result );
	}
	free( ctx );
}

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return GetCurrCo(env);
}



typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{
    if (timeout == 0)
	{
		return pollfunc(fds, nfds, timeout);
	}
	if (timeout < 0)
	{
		timeout = INT_MAX;
	}
	int epfd = ctx->iEpollFd;
	stCoRoutine_t* self = co_self();

	//1.struct change
	stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t)));
	memset( &arg,0,sizeof(arg) );

	arg.iEpollFd = epfd;
	arg.fds = (pollfd*)calloc(nfds, sizeof(pollfd));
	arg.nfds = nfds;

	stPollItem_t arr[2];
	if( nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack)
	{
		arg.pPollItems = arr;
	}	
	else
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );

	arg.pfnProcess = OnPollProcessEvent;
	arg.pArg = GetCurrCo( co_get_curr_thread_env() );
	
	
	//2. add epoll
	for(nfds_t i=0;i<nfds;i++)
	{
		arg.pPollItems[i].pSelf = arg.fds + i;
		arg.pPollItems[i].pPoll = &arg;

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if( fds[i].fd > -1 )
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll( fds[i].events );

			int ret = co_epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
			if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL)
			{
				if( arg.pPollItems != arr )
				{
					free( arg.pPollItems );
					arg.pPollItems = NULL;
				}
				free(arg.fds);
				free(&arg);
				return pollfunc(fds, nfds, timeout);
			}
		}
		//if fail,the timeout would work
	}

	//3.add timeout

	unsigned long long now = GetTickMS();
	arg.ullExpireTime = now + timeout;
	int ret = AddTimeout( ctx->pTimeout,&arg,now );
	int iRaiseCnt = 0;
	if( ret != 0 )
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
				ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		iRaiseCnt = -1;

	}
    else
	{
		co_yield_env( co_get_curr_thread_env() );
		iRaiseCnt = arg.iRaiseCnt;
	}

    {
		//clear epoll status and memory
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
		for(nfds_t i = 0;i < nfds;i++)
		{
			int fd = fds[i].fd;
			if( fd > -1 )
			{
				co_epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent );
			}
			fds[i].revents = arg.fds[i].revents;
		}


		if( arg.pPollItems != arr )
		{
			free( arg.pPollItems );
			arg.pPollItems = NULL;
		}

		free(arg.fds);
		free(&arg);
	}

	return iRaiseCnt;
}

int	co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms )
{
	return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum 
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value;
	return 0;
}



void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}

stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}

//co cond
struct stCoCond_t;
struct stCoCondItem_t 
{
	stCoCondItem_t *pPrev;
	stCoCondItem_t *pNext;
	stCoCond_t *pLink;

	stTimeoutItem_t timeout;
};
struct stCoCond_t
{
	stCoCondItem_t *head;
	stCoCondItem_t *tail;
};
static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

stCoCondItem_t *co_cond_pop( stCoCond_t *link );
int co_cond_signal( stCoCond_t *si )
{
	stCoCondItem_t * sp = co_cond_pop( si );
	if( !sp ) 
	{
		return 0;
	}
	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

	AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );

	return 0;
}
int co_cond_broadcast( stCoCond_t *si )
{
	for(;;)
	{
		stCoCondItem_t * sp = co_cond_pop( si );
		if( !sp ) return 0;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

		AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
	}

	return 0;
}


int co_cond_timedwait( stCoCond_t *link,int ms )
{
	stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1, sizeof(stCoCondItem_t));
	psi->timeout.pArg = GetCurrThreadCo();
	psi->timeout.pfnProcess = OnSignalProcessEvent;

	if( ms > 0 )
	{
		unsigned long long now = GetTickMS();
		psi->timeout.ullExpireTime = now + ms;

		int ret = AddTimeout( co_get_curr_thread_env()->pEpoll->pTimeout,&psi->timeout,now );
		if( ret != 0 )
		{
			free(psi);
			return ret;
		}
	}
	AddTail( link, psi);

	co_yield_ct();


	RemoveFromLink<stCoCondItem_t,stCoCond_t>( psi );
	free(psi);

	return 0;
}
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t*)calloc( 1,sizeof(stCoCond_t) );
}
int co_cond_free( stCoCond_t * cc )
{
	free( cc );
	return 0;
}


stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
	stCoCondItem_t *p = link->head;
	if( p )
	{
		PopHead<stCoCondItem_t,stCoCond_t>( link );
	}
	return p;
}
