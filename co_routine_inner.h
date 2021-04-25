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


#ifndef __CO_ROUTINE_INNER_H__
#define __CO_ROUTINE_INNER_H__

#include "co_routine.h"
#include "coctx.h"
struct stCoRoutineEnv_t;
struct stCoSpec_t
{
	void *value;
};

struct stStackMem_t
{
    stCoRoutine_t* occupy_co; // 执行时占用的那个协程实体,也就是这个栈现在是那个协程在用
    int stack_size;			//当前栈上未使用的空间
    char* stack_bp; 		//stack_buffer + stack_size
    char* stack_buffer;		//栈的起始地址,当然对于主协程来说这是堆上的空间


    /* 在co_alloc_stackmem中
    stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
    stack_mem->occupy_co= NULL;
    stack_mem->stack_size = stack_size;
    stack_mem->stack_buffer = (char*)malloc(stack_size);
    stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
     */
};

// 共享栈中多栈可以使得我们在进程切换的时候减少拷贝次数
struct stShareStack_t
{
    unsigned int alloc_idx;		// stack_array中我们在下一次调用中应该使用的那个共享栈的index
    int stack_size;				// 共享栈的大小，这里的大小指的是一个stStackMem_t*的大小
    int count;					// 共享栈的个数，共享栈可以为多个，所以以下为共享栈的数组
    stStackMem_t** stack_array;	// 栈的内容，这里是个数组，元素是stStackMem_t*
};
/*
基本注释都说清楚了，stStackMem_t没什么说的。我们来看看stShareStack_t，其中有一个参数count，是共享栈的个数。共享栈为什么还能有多个？这是一个对于共享栈的优化，可以减少内容的拷贝数。我们知道共享栈在切换协程的时候会执行拷贝，把要切换出去的协程的栈内容进行拷贝，但是如果要当前协程和要切换的协程所使用的栈不同，拷贝这一步当然就可以省略了。

我们来看看在co_create中的co_get_stackmem是如何在共享栈结构中分配栈的：
*/



// libco的协程一旦创建之后便和创建它的线程绑定在一起 不支持线程之间的迁移
struct stCoRoutine_t
{
    stCoRoutineEnv_t *env; // 协程的执行环境,运行在同一个线程上的各协程是共享该结构
    pfn_co_routine_t pfn;  // 结构为一个函数指针 实际待执行的协程函数


    void *arg; // pfn的参数
    // 用于协程切换时保存 CPU 上下文（context）的,即 esp、ebp、eip 和其他通用寄存器的值
    coctx_t ctx;

    // 一些状态和标志变量
    char cStart; // 协程是否执行过resume
    char cEnd;
    char cIsMain; //是否为主协程 在co_init_curr_thread_env修改
    char cEnableSysHook; //此协程是否hook库函数
    char cIsShareStack;	 // 是否开启共享栈模式

    // 保存程序系统环境变量的指针
    void *pvEnv;


    //这里也可以看出libco协程是stackful的,也有一些库的实现是stackless,即无栈协程
    // char sRunStack[ 1024 * 128 ];
    // 协程运行时的栈内存
    stStackMem_t* stack_mem;

    /**
     * 一个协程实际占用的（从 esp 到栈底）栈空间，相比预分配的这个栈大小
     * （比如 libco 的 128KB）会小得多；这样一来， copying stack
     *  的实现方案所占用的内存便会少很多。当然，协程切换时拷贝内存的开销
     *  有些场景下也是很大的。因此两种方案各有利弊，而 libco 则同时实现
     *  了两种方案，默认使用前者
     */

    // save satck buffer while confilct on same stack_buffer;
    // 当使用共享栈的时候需要用到的一些数据结构
    char* stack_sp;
    unsigned int save_size;
    char* save_buffer;

    stCoSpec_t aSpec[1024];

};
/*
env是一个非常关键的结构，这个结构是所有数据中最特殊的一个，因为它是一个线程内共享的结构，也就是说同一个线程创建的所有协程的此结构指针指向同一个数据。其中存放了一些协程调度相关的数据，当然叫调度有些勉强，因为libco实现的非对称式协程实际上没有什么调度策略，完全就是协程切换会调用这个协程的协程或者线程。这个结构我们会在后面仔细讲解。
pfn是一个函数指针，类型为function<void*(void*)>，当然libco虽然是用C++写的，但是整体风格偏向于C语言，所以实际结构是一个函数指针。值得一提的是实际存储的函数指针并不是我们传入的函数指针，而是一个使用我们传入的函数指针的一个函数，原因是当协程执行完毕的时候需要切换CPU执行权，这样可以做到最小化入侵用户代码。
arg没什么说的，传入的指针的参数。
ctx保存协程的上下文，实际就是寄存器的值，不管是C还是C++都没有函数可以直接接触寄存器，所以操作这个参数的时候需要嵌入一点汇编代码。
紧接着是五个标记位，功能注释中写的很清楚啦。
pvEnv保存着环境变量相关，这个环境变量其实是与hook后的setenv，getenv类函数有关。和上面说的env没有什么关系。
stack_mem是运行是栈的结构，libco提供了两种方式，一个是每个协程拥有一个独立的栈，默认分配128KB空间，缺点是每个协程可能只用到了1KB不到，碎片较多。还有一种是共享栈模式，需要我们在创建协程的时候在Co_create中指定第二个参数，这种方法是多个协程共用一个栈，但是在协程切换的时候需要拷贝已使用的栈空间。
剩下的就是一些在共享栈时要用到的参数了。
*/



//1.env
void 				co_init_curr_thread_env();
stCoRoutineEnv_t *	co_get_curr_thread_env();

//2.coroutine
void    co_free( stCoRoutine_t * co );
void    co_yield_env(  stCoRoutineEnv_t *env );

//3.func



//-----------------------------------------------------------------------------------------------

struct stTimeout_t;
struct stTimeoutItem_t ;

stTimeout_t *AllocTimeout( int iSize );
void 	FreeTimeout( stTimeout_t *apTimeout );
int  	AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,uint64_t allNow );

struct stCoEpoll_t;
stCoEpoll_t * AllocEpoll();
void 		FreeEpoll( stCoEpoll_t *ctx );

stCoRoutine_t *		GetCurrThreadCo();
void 				SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev );

typedef void (*pfnCoRoutineFunc_t)();

#endif