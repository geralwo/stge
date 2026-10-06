#include <stdlib.h>
#include <string.h>

#include "utils/dyn.h"

void dyn_init(Dyn *a, uint32_t size_of_element)
{
	if (a == NULL)
		return;

	a->data = NULL;
	a->len = 0;
	a->cap = 0;
	a->size_of_element = size_of_element == 0 ? 1 : size_of_element;
}

bool dyn_add(Dyn *a, const void *elem)
{
	void *grown;
	uint32_t cap;
	uint8_t *slot;

	if (a == NULL || elem == NULL || a->size_of_element == 0)
		return false;

	if (a->len >= a->cap)
	{
		cap = a->cap == 0 ? 8 : a->cap * 2;
		grown = realloc(a->data, (size_t)cap * a->size_of_element);
		if (grown == NULL)
			return false;

		a->data = grown;
		a->cap = cap;
	}

	slot = (uint8_t *)a->data + (size_t)a->len * a->size_of_element;
	memcpy(slot, elem, a->size_of_element);
	a->len++;
	return true;
}

void *dyn_at(Dyn *a, uint32_t i)
{
	if (a == NULL || a->data == NULL || i >= a->len)
		return NULL;

	return (uint8_t *)a->data + (size_t)i * a->size_of_element;
}

uint32_t dyn_len(const Dyn *a)
{
	if (a == NULL)
		return 0;

	return a->len;
}

void dyn_free(Dyn *a)
{
	if (a == NULL)
		return;

	free(a->data);
	a->data = NULL;
	a->len = 0;
	a->cap = 0;
}
