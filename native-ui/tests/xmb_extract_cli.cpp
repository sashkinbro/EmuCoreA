// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#include "xmb_extract.h"
#include <cstdio>
#include <chrono>
int main(int argc,char**argv){ if(argc<3) return 2; auto t=std::chrono::steady_clock::now();
 std::string e=xmb::ExtractResources(argv[1],argv[2]);
 double s=std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();
 printf("result: '%s' in %.2fs\n",e.c_str(),s); return e.empty()?0:1;}
