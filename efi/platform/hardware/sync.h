#pragma once
#ifndef __dmb
#define __dmb() __asm__ __volatile__("mfence" ::: "memory")
#endif
#define __mem_fence() __dmb()
