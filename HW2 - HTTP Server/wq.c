#include <stdlib.h>
#include "wq.h"
#include "utlist.h"

pthread_mutex_t lock;
pthread_cond_t has_work;

/* Initializes a work queue WQ. */
void wq_init(wq_t *wq, void (*request_handler)(int)) {
  pthread_mutex_init(&lock, NULL);
  pthread_cond_init(&has_work, NULL);
  wq->size = 0;
  wq->head = NULL;
  wq->request_handler = request_handler;
}

/* Remove an item from the WQ. This function should block until there
 * is at least one item on the queue. */
int wq_pop(wq_t *wq) {
  pthread_mutex_lock(&lock);
  
  while (wq->size == 0) {
    pthread_cond_wait(&has_work, &lock);
  }

  wq_item_t *wq_item = wq->head;
  int client_socket_fd = wq->head->client_socket_fd;
  wq->size--;
  DL_DELETE(wq->head, wq->head);

  free(wq_item);
  pthread_mutex_unlock(&lock);
  return client_socket_fd;
}

/* Add ITEM to WQ. */
void wq_push(wq_t *wq, int client_socket_fd) {
  pthread_mutex_lock(&lock);
  wq_item_t *wq_item = calloc(1, sizeof(wq_item_t));
  wq_item->client_socket_fd = client_socket_fd;
  DL_APPEND(wq->head, wq_item);
  wq->size++;
  if (wq->size == 1) {
    pthread_cond_signal(&has_work);
  }
  pthread_mutex_unlock(&lock);
}
