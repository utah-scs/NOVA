// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "shm.h"

CommandResponse SHM::Init(const bess::pb::EmptyArg &) {
	int res;
	int fd;

	// get shared memory file descriptor (NOT a file)
	fd = shm_open(SHM_ID, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd == -1)
    CommandFailure(EINVAL, "shm_open failed");

	// extend shared memory object as by default it's initialized with size 0
	res = ftruncate(fd, SHM_SIZE);
	if (res == -1)
    CommandFailure(EINVAL, "ftruncate failed");

	// map shared memory to process address space
	addr = static_cast<char *>(mmap((caddr_t)0, SHM_SIZE, PROT_WRITE, MAP_SHARED, fd, 0));
	if (addr == MAP_FAILED)
    CommandFailure(EINVAL, "mmap failed");

  return CommandSuccess();
}

void SHM::DeInit() {
  int fd;
  int res;

	// mmap cleanup
	res = munmap(addr, SHM_SIZE);
	if (res == -1)
    CommandFailure(EINVAL, "munmap failed");

	// shm_open cleanup
	fd = shm_unlink(SHM_ID);
	if (fd == -1)
    CommandFailure(EINVAL, "shm_unlink failed");
}

void SHM::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int len;
  char num_str[SHM_SIZE];
  int cnt = batch->cnt();

  for(int i = 0; i < cnt; i++) {
    counter++; 
    sprintf(num_str, "%llu", counter);
    len = strlen(num_str) + 1;
    memcpy(addr, num_str, len);
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(SHM, "shm", "Shared memory test")
