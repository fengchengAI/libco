/*
* Tencent is pleased to support the open source community by making Libco
available.

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

#include "coctx.h"
#include <stdio.h>
#include <string.h>

#define ESP 0
#define EIP 1
#define EAX 2
#define ECX 3
// -----------
#define RSP 0
#define RIP 1
#define RBX 2
#define RDI 3
#define RSI 4

#define RBP 5
#define R12 6
#define R13 7
#define R14 8
#define R15 9
#define RDX 10
#define RCX 11
#define R8 12
#define R9 13


//-------------
// 64 bit
// low | regs[0]: r15 |
//    | regs[1]: r14 |
//    | regs[2]: r13 |
//    | regs[3]: r12 |
//    | regs[4]: r9  |
//    | regs[5]: r8  |
//    | regs[6]: rbp |
//    | regs[7]: rdi |
//    | regs[8]: rsi |
//    | regs[9]: ret |  //ret func addr
//    | regs[10]: rdx |
//    | regs[11]: rcx |
//    | regs[12]: rbx |
// hig | regs[13]: rsp |
enum {
  kRDI = 7,
  kRSI = 8,
  kRETAddr = 9,
  kRSP = 13,
};

//这里面对于协程切换来说最重要的就是regs[0]和regs[kRSP]了，regs[0] 存放下一个指令执行地址，也即返回地址。
//regs[kRSP] 存放切换到新协程后，kRSP，也就是栈上的偏移。这样程序的数据和代码都被改变，当然也就做到了一个线程可以跑多份代码了。

// 64 bit
extern "C" {
extern void coctx_swap(coctx_t*, coctx_t*) asm("coctx_swap");
};

int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {
    // 将s,s1 和pfn绑定到ctx中
    // 此时sp其实就是esp指向的地方 其中ss_size感觉像是这个栈上目前剩余的空间,
    char* sp = ctx->ss_sp + ctx->ss_size - sizeof(coctx_param_t);

    //auto aa = ctx->ss_sp + ctx->ss_size;
    sp = (char*)((unsigned long)sp & -16L);// 字节对齐


    //memset(ctx->regs, 0, sizeof(ctx->regs));
    bzero(ctx->regs, sizeof(ctx->regs));
    void** ret_addr = (void**)(sp);
    *ret_addr = (void*)pfn;

    ctx->regs[kRSP] = sp;
    ctx->regs[kRETAddr] = (char*)pfn;
    ctx->regs[kRDI] = (char*)s;
    ctx->regs[kRSI] = (char*)s1;
    return 0;
}


int coctx_init(coctx_t* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  return 0;
}

