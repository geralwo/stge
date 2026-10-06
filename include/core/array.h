#ifndef STGE_CORE_ARRAY_H
#define STGE_CORE_ARRAY_H

#include <stdint.h>

/* Growable T[] / byte[] only. Fixed T[n] is packed elements with no header. */
typedef struct
{
	uint32_t length;
	uint32_t size_of_element;
	uint8_t *data;
} Array;

#endif /* STGE_CORE_ARRAY_H */
