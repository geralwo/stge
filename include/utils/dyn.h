#ifndef STGE_UTILS_DYN_H
#define STGE_UTILS_DYN_H

#include <stdbool.h>
#include <stdint.h>

/* Growable store for script T[] arrays only. */
typedef struct
{
	void *data;
	uint32_t len;
	uint32_t cap;
	uint32_t size_of_element;
} Dyn;

void dyn_init(Dyn *a, uint32_t size_of_element);
bool dyn_add(Dyn *a, const void *elem);
void *dyn_at(Dyn *a, uint32_t i);
uint32_t dyn_len(const Dyn *a);
void dyn_free(Dyn *a);

#endif /* STGE_UTILS_DYN_H */
