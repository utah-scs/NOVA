#ifndef __COMMON_H__
#define __COMMON_H__

typedef struct __attribute__((packed)) get_set_req {
  uint8_t op_type;
  uint64_t mr_offset;
  uint64_t data_size;
} get_set_req_t;

#endif // __COMMON_H__
