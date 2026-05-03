// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
#include "io.h"

 /*
 * Important: This program will execute on the UART device, meaning the screen
 * has to be open before the program is ran!! An easy way to do this is by using
 * GDB to start and halt program execution.
 */
void main(struct io * termio) {
    char buf[15] = "Hello, world!";
    iowrite(termio, buf,  15);
}