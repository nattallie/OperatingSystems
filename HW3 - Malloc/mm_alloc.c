/*
 * mm_alloc.c
 *
 * Stub implementations of the mm_* routines.
 */

#include "mm_alloc.h"
#include <stdlib.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

struct metadata {
    size_t size;
    bool free;
    struct metadata* next;
    struct metadata* prev;
};

void fill_metadata_info(struct metadata* data, size_t size);
void* find_block_of_size(size_t size);
void check_for_split(struct metadata* ptr, size_t wanted_size);

void* head_block = NULL;
char* heap_start = NULL;
struct metadata* last_block = NULL;

void fill_metadata_info(struct metadata* data, size_t size) {
    data->size = size;
    data->free = false;
    data->next = NULL;
    data->prev = last_block;
    if (last_block != NULL) {
        last_block->next = data;
    }
    last_block = data;
    if (head_block == NULL) {
        head_block = data;
    }
}

void check_for_split(struct metadata* ptr, size_t wanted_size) {
    if (ptr->size - wanted_size > sizeof(struct metadata)) {
        struct metadata* new_block = (struct metadata*)((char*)ptr + wanted_size + sizeof(struct metadata));
        new_block->next = ptr->next;
        new_block->prev = ptr;
        ptr->next = new_block;
        new_block->size = ptr->size - wanted_size - sizeof(struct metadata);
        new_block->free = true;
        ptr->size = ptr->size - wanted_size;
        ptr->free = false;
    }
}

void* find_block_of_size(size_t size) {   
    struct metadata* cur_data = head_block;

    while (cur_data != NULL) {
        if (cur_data->free && cur_data->size >= size) {
            check_for_split(cur_data, size);
            return cur_data;
        }
        cur_data = cur_data->next;
    } 

    return NULL;
}


void *mm_malloc(size_t size) {
    void* res_ptr = NULL;
    if (size == 0) 
        return NULL;
    
    void* new_block = find_block_of_size(size); 
    if (new_block != NULL) {
        res_ptr = new_block; 
    } else {
        res_ptr = sbrk(0);
        int num_allocated = (int) sbrk(size + sizeof(struct metadata));
        
        if (num_allocated < 0) 
            return NULL;
    
        fill_metadata_info((struct metadata*)res_ptr, size);
    }

    res_ptr = (char*)res_ptr + sizeof(struct metadata);

    for (int k = 0; k < size; k++) {
        *((char*)res_ptr) = 0;
    }

    ((struct metadata*)res_ptr)->free = false;
    return res_ptr;
}

void *mm_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return mm_malloc(size);
    }

    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }

    ptr = (char*)ptr - sizeof(struct metadata);

    if (((struct metadata*)ptr)->size > size) {
        check_for_split(ptr, size);
        return ((char*)ptr + sizeof(struct metadata));
    }

    void* new_ptr = mm_malloc(size);
    if (new_ptr == NULL)
        return NULL;
    

    ptr = (char*)ptr + sizeof(struct metadata);
    memcpy(new_ptr, ptr, size);
    mm_free(ptr);
    return new_ptr;
}

void mm_free(void *ptr) {
    char* cur_ptr = head_block;
    struct metadata* next_ptr = NULL;
    struct metadata* prev_ptr = NULL;
    while (cur_ptr != NULL) {
        if (cur_ptr + sizeof(struct metadata) == ptr) {
            ((struct metadata*)cur_ptr)->free = true;
            next_ptr = ((struct metadata*)cur_ptr)->next;
            while (next_ptr != NULL && next_ptr->free) {
                ((struct metadata*)cur_ptr)->size += next_ptr->size + sizeof(struct metadata);
                ((struct metadata*)cur_ptr)->next = next_ptr->next;
                next_ptr = next_ptr->next;
            }

            prev_ptr = ((struct metadata*)cur_ptr)->prev; 
            while (prev_ptr != NULL && prev_ptr->free) {
                ((struct metadata*)prev_ptr)->size += ((struct metadata*)cur_ptr)->size + sizeof(struct metadata);
                ((struct metadata*)prev_ptr)->next = ((struct metadata*)cur_ptr)->next;
                cur_ptr = (char*)prev_ptr;
                prev_ptr = ((struct metadata*)prev_ptr)->prev;
            }

            return;
        }
        cur_ptr = (char*)(((struct metadata*)cur_ptr)->next);
    }
}
