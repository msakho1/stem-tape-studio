/*
 * st_library_io.h — torn-write-safe load/save of the Stem Tape library
 * header (st_library_header_t, see st_storage_layout.h), through the SAME
 * injected sector I/O convention st_transfer.h uses (st_sector_write_fn /
 * st_sector_read_fn) -- the real firmware binds these to eMMC-backed
 * sector read/write (see main.c); host tests bind them to an in-memory
 * mock, so the actual torn-write guarantee (a power loss mid-save can
 * never leave BOTH copies invalid, and a corrupted single copy is
 * transparently recovered from the other) is proven here, not just
 * documented.
 *
 * Two redundant copies, each ST_LIBRARY_HEADER_SECTORS_PER_COPY sectors,
 * starting at ST_LIBRARY_HEADER_SECTOR0 -- copy 0 first, copy 1
 * immediately after. A reader trusts whichever copy deserializes validly
 * (magic + in-bounds slot_count + matching header_crc32) AND has the
 * higher `generation` (wraparound-safe comparison); a writer always
 * updates the OTHER (currently-untrusted, or lower-generation) copy
 * FIRST, then the previously-trusted copy SECOND -- so a power loss
 * between the two writes always leaves the previously-trusted copy intact
 * and still selected as trusted on the next load.
 *
 * PURE: no Zephyr, no direct eMMC driver calls.
 */

#ifndef STEMTAPE_PLAYER_LIBRARY_IO_H_
#define STEMTAPE_PLAYER_LIBRARY_IO_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_storage_layout.h"
#include "st_transfer.h" /* st_sector_write_fn / st_sector_read_fn */

typedef enum {
	ST_LIBIO_LOADED = 0,     /* a valid, trusted copy was found and *out is it */
	ST_LIBIO_FRESH,          /* neither copy validated -- *out is a fresh, empty
				  * library (magic/layout_version/slot_count set,
				  * generation 0, every slot zeroed); NOT an error --
				  * this is the expected result on a freshly
				  * provisioned card, exactly like the classic
				  * looper's own "unrecognized -> safe empty state"
				  * pattern, except here the caller MAY go on to
				  * legitimately format/write it (unlike Phase 1's
				  * permanently read-only classic-format handling). */
	ST_LIBIO_READ_FAILED,    /* the injected read_fn itself failed for BOTH
				  * copies (a real I/O error, not just invalid
				  * content) -- *out is unmodified; the caller
				  * must not treat this the same as ST_LIBIO_FRESH
				  * (a real read failure must not be silently
				  * reinterpreted as "safe to format-fresh") */
} st_libio_load_result_t;

/*
 * Loads the trusted copy (see file header). `slot_count_if_fresh` seeds
 * *out's slot_count when neither copy validates (ST_LIBIO_FRESH) --
 * callers pass the real, capacity-detected value (see
 * st_storage_compute_slot_capacity()), never a placeholder. `trusted_copy`
 * receives which copy (0 or 1) was selected as trusted, or -1 if neither
 * validated (LOADED only ever reports 0 or 1; FRESH/READ_FAILED report
 * -1) -- st_libio_save() needs this to decide write order.
 */
st_libio_load_result_t st_libio_load(st_library_header_t *out, uint32_t slot_count_if_fresh,
				      int *trusted_copy, st_sector_read_fn read_fn, void *ctx);

/*
 * Saves `h` as a NEW generation (increments h->generation itself, so the
 * caller passes the header as it should read AFTER this call, not a
 * pre-bumped one) using the torn-write-safe write order described above.
 * `trusted_copy` is the value st_libio_load() reported (0, 1, or -1 for
 * "neither copy currently valid, order doesn't matter" -- writes copy 0
 * then copy 1 in that case). Returns false (leaves the on-media state
 * exactly as far as it got -- see the file header's guarantee) on any
 * write_fn failure; true only if BOTH copies were written successfully.
 */
bool st_libio_save(st_library_header_t *h, int trusted_copy,
		    st_sector_write_fn write_fn, void *ctx);

#endif /* STEMTAPE_PLAYER_LIBRARY_IO_H_ */
