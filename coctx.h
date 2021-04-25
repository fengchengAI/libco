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

#ifndef __CO_CTX_H__
#define __CO_CTX_H__
#include <stdlib.h>
typedef void* (*coctx_pfn_t)( void* s, void* s2 );
struct coctx_param_t
{
    //指向stCoRoutine_t *，分别表示，新旧协程
	const void *s1;  //新的
	const void *s2;
};
struct coctx_t
{

	void *regs[ 14 ];  //指针数组,有14个void* 类型的数,void * 是八字节的
	size_t ss_size;
	char *ss_sp;  //这个协程栈的基址
	
};
/*
   x86-64的16个64位寄存器分别是：%rax, %rbx, %rcx, %rdx, %esi, %edi, %rbp, %rsp, %r8-%r15。其中：

    %rax 作为函数返回值使用
    %rsp栈指针寄存器，指向栈顶
    %rdi，%rsi，%rdx，%rcx，%r8，%r9 用作函数参数，依次对应第1参数，第2参数
    %rbx，%rbp，%r12，%r13，%14，%15 用作数据存储，遵循被调用者保护规则，简单说就是随便用，调用子函数之前要备份它，以防被修改
    %r10，%r11 用作数据存储，遵循调用者保护规则，简单说就是使用之前要先保存原值

我们来看看两个陌生的名词调用者保护&被调用者保护：

    调用者保护：表示这些寄存器上存储的值，需要调用者(父函数)自己想办法先备份好，否则过会子函数直接使用这些寄存器将无情的覆盖。如何备份？当然是实现压栈(pushl),等子函数调用完成，再通过栈恢复(popl)
    被调用者保护：即表示需要由被调用者(子函数)想办法帮调用者(父函数)进行备份

 */

int coctx_init( coctx_t *ctx );
int coctx_make( coctx_t *ctx,coctx_pfn_t pfn,const void *s,const void *s1 );
#endif
