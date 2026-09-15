#include <inttypes.h>
#include "naam.h"
#include "compose_post.h"

SECTION("xdp")
uint64_t compose_post(struct xdp_md *ctx) {
  uint64_t unique_id;
  uint64_t offset;
  uint64_t memreg_id;
  uint64_t user_id;
  dma_addr_t from_addr;
  dma_addr_t to_addr;
  
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 

  if (pkt + sizeof(req_pkt_t) + sizeof(compose_post_req_t) > pkt_end)
    return 1;
  
  compose_post_req_t *post = (compose_post_req_t *)APP_REGION_PTR(pkt);

  // TODO: create unique id for the post
  post->post_id = unique_id;
  
  // Get timestamp
  post->created_at = bpf_ktime_get_ns();
    
  /*
  * TODO:
  * - Use regular expression to find mentions and URLs from text
  * - Check if mentioned users exist
  * - Shorten URLs and modify text with shortened URLs
  *
  * If implemented text will be modified by shortened URLs
  */
  // post.text = text;
  // post.urls = urls;
  // post.mentions = mentions;

  /*
   * At this point the post is composed
   * Store post in post database
   */

  // Allocate space in the DB for this post
  offset = DMA_FAA(pkt, DMA_ADDR(1, POST_DB_MEMREG_ID, 0), sizeof(compose_post_req_t));

  // Insert the post in the DB
  from_addr = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt));
  to_addr = DMA_ADDR(1, POST_DB_MEMREG_ID, offset);
  DMA_WRITE(pkt, to_addr, from_addr, sizeof(compose_post_req_t));
  // Inster the offset to the post in KV store
  // This can be retrieved b the post_id to fetch the post later from the DB
  mica_set(pkt, POST_KV_MEMREG_ID, APP_REGION_OFFSET(pkt) + sizeof(compose_post_req_t), post->post_id, offset);
  
  /*
   * Update user timeline database
   * A KV store is used to store the memory region ID of the user's timeline database by user_id
   * The user's timeline database is a list of post IDs
   */
  
  // Fech the memory region ID for this user's timeline database
  mica_get(pkt, USER_TIMELINE_KV_MEMREG_ID, APP_REGION_OFFSET(pkt) + sizeof(compose_post_req_t), post->user_id, &memreg_id);
  
  // Allocate space in the DB for this post
  offset = DMA_FAA(pkt, DMA_ADDR(1, memreg_id, 0), sizeof(((compose_post_req_t *)0)->post_id));
  
  // Insert the post id in the user's timeline db
  from_addr = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + offsetof(compose_post_req_t, post_id));
  to_addr = DMA_ADDR(1, memreg_id, offset);
  DMA_WRITE(pkt, to_addr, from_addr, sizeof(((compose_post_req_t *)0)->post_id));

  /*
   * Get list of followers of the user from the social graph database
   * Update the timeline database of the followers of the user
   */
  
  // Fech the memory region ID for this user's social graph database
  mica_get(pkt, SOCIAL_GRAPH_KV_MEMREG_ID, APP_REGION_OFFSET(pkt) + sizeof(compose_post_req_t), post->user_id, &memreg_id);

  // Fetch the followers of the user from user's social graph database
  // First 8 bytes contain the number of followers
  // Next series of 8 bytes contain the user IDs of the followers
  from_addr = DMA_ADDR(1, memreg_id, 0);
  to_addr = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + offsetof(compose_post_req_t, post_id));
  DMA_READ(pkt, to_addr, from_addr, sizeof(uint64_t) * MAX_FOLLOWERS);

  // iterate over the followers and update their home timeline database
  // First 8 bytes contain the number of followers; skip this
  for (int i = 1; i < MAX_FOLLOWERS + 1; i++) {
    user_id = *((uint64_t *)((char *)APP_REGION_PTR(pkt) + sizeof(compose_post_req_t)) + i);
    // If user_id is 0, then there are no more followers
    if (user_id == 0) {
      break;
    }

    // Fech the memory region ID for this user's timeline database
    mica_get(pkt, HOME_TIMELINE_KV_MEMREG_ID, APP_REGION_OFFSET(pkt) + sizeof(compose_post_req_t), post->user_id, &memreg_id);
    
    // Allocate space in the DB for this post
    offset = DMA_FAA(pkt, DMA_ADDR(1, memreg_id, 0), sizeof(((compose_post_req_t *)0)->post_id));
    
    // Insert the post id in the user's timeline db
    from_addr = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + offsetof(compose_post_req_t, post_id));
    to_addr = DMA_ADDR(1, memreg_id, offset);
    DMA_WRITE(pkt, to_addr, from_addr, sizeof(((compose_post_req_t *)0)->post_id));
  }

  return 0;
}
