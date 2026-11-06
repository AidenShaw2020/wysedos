/* Host-only dependency seam: the interpreter must never sleep in this test. */
#pragma once
int usleep(unsigned int usec);