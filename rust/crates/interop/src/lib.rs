//! C ABI shared by the Linux C++ module and browser WebAssembly smoke tests.

use std::mem::{MaybeUninit, size_of};
use std::slice;

const OK: u32 = 0;
const INVALID_BUFFER: u32 = 1;

/// Returns the revision of the Rust interop ABI.
// SAFETY: This crate owns the unique `hermes_rust_abi_version` symbol.
#[unsafe(no_mangle)]
pub extern "C" fn hermes_rust_abi_version() -> u32 {
    1
}

/// Returns the forward distance in ticks between 32-bit RTP timestamps.
///
/// The subtraction wraps at 2³². The caller determines packet ordering.
// SAFETY: This crate owns the unique `hermes_rust_rtp_timestamp_distance` symbol.
#[unsafe(no_mangle)]
pub extern "C" fn hermes_rust_rtp_timestamp_distance(earlier: u32, later: u32) -> u32 {
    hermes_rtp::timestamp_distance(earlier, later)
}

/// Writes each forward distance from `earlier` to the caller's output buffer.
///
/// Status 0 means success. The counts must match and are measured in elements.
/// Equal zero counts succeed without accessing the pointers. Status 1 means an
/// invalid count, null or misaligned nonempty pointer, or overlapping range.
/// The function returns before writing any output on error.
///
/// # Safety
///
/// For nonempty buffers that pass validation, each pointer must designate a
/// range within one live allocation. The input must contain initialized
/// values and remain immutable throughout the call. The caller must grant
/// exclusive access to the writable output range, which may be uninitialized.
/// Both buffers remain caller-owned. The function retains neither pointer.
// SAFETY: This crate owns the unique `hermes_rust_rtp_timestamp_distances` symbol.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hermes_rust_rtp_timestamp_distances(
    earlier: u32,
    later: *const u32,
    later_count: usize,
    distances: *mut u32,
    distance_count: usize,
) -> u32 {
    if later_count != distance_count {
        return INVALID_BUFFER;
    }
    if later_count == 0 {
        return OK;
    }
    if later.is_null() || distances.is_null() || !later.is_aligned() || !distances.is_aligned() {
        return INVALID_BUFFER;
    }

    let Some(byte_count) = later_count
        .checked_mul(size_of::<u32>())
        .filter(|&bytes| bytes <= isize::MAX as usize)
    else {
        return INVALID_BUFFER;
    };
    let (input_start, output_start) = (later.addr(), distances.addr());
    let (Some(input_end), Some(output_end)) = (
        input_start.checked_add(byte_count),
        output_start.checked_add(byte_count),
    ) else {
        return INVALID_BUFFER;
    };
    if input_start < output_end && output_start < input_end {
        return INVALID_BUFFER;
    }

    // SAFETY: Both pointers are aligned, non-null and have bounded, disjoint
    // ranges. The caller guarantees allocation validity and the documented
    // access rules. MaybeUninit permits writing without reading the output.
    let (later, distances) = unsafe {
        (
            slice::from_raw_parts(later, later_count),
            slice::from_raw_parts_mut(distances.cast::<MaybeUninit<u32>>(), distance_count),
        )
    };
    for (output, distance) in distances
        .iter_mut()
        .zip(hermes_rtp::timestamp_distances(earlier, later))
    {
        output.write(distance);
    }
    OK
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr::{null, null_mut, without_provenance};

    #[test]
    fn reports_abi_revision() {
        assert_eq!(hermes_rust_abi_version(), 1);
    }

    #[test]
    fn measures_forward_rtp_timestamp_distance() {
        assert_eq!(hermes_rust_rtp_timestamp_distance(u32::MAX - 1, 1), 3);
    }

    #[test]
    fn writes_distances_to_caller_storage() {
        let timestamps = [u32::MAX - 1, u32::MAX, 0, 1, 0x7fff_fffe];
        let mut output = [MaybeUninit::<u32>::uninit(); 5];

        // SAFETY: Both buffers are live and disjoint. The output is writable.
        let status = unsafe {
            hermes_rust_rtp_timestamp_distances(
                u32::MAX - 1,
                timestamps.as_ptr(),
                timestamps.len(),
                output.as_mut_ptr().cast(),
                output.len(),
            )
        };

        assert_eq!(status, OK);
        // SAFETY: A successful call initializes every output element.
        let output = output.map(|distance| unsafe { distance.assume_init() });
        assert_eq!(output, [0, 1, 2, 3, 0x8000_0000]);
    }

    #[test]
    fn empty_accepts_null_buffers() {
        // SAFETY: The zero counts allow null pointers.
        let status = unsafe { hermes_rust_rtp_timestamp_distances(0, null(), 0, null_mut(), 0) };
        assert_eq!(status, OK);
    }

    #[test]
    fn rejects_malformed_ranges_before_writing() {
        let timestamps = [1_u32, 2, 3];
        let mut output = [42_u32; 3];
        let input = timestamps.as_ptr();
        let output_pointer = output.as_mut_ptr();
        let misaligned = input.cast::<u8>().wrapping_add(1).cast();
        let wrapping = without_provenance::<u32>(usize::MAX & !3);
        let oversized = isize::MAX as usize / size_of::<u32>() + 1;

        for (input, input_count, output, output_count) in [
            (input, 2, output_pointer, 3),
            (null(), 3, output_pointer, 3),
            (input, 3, null_mut(), 3),
            (misaligned, 3, output_pointer, 3),
            (wrapping, 3, output_pointer, 3),
            (input, oversized, output_pointer, oversized),
            (input, usize::MAX, output_pointer, usize::MAX),
        ] {
            // SAFETY: Each invalid range fails validation without pointer access.
            let status = unsafe {
                hermes_rust_rtp_timestamp_distances(0, input, input_count, output, output_count)
            };
            assert_eq!(status, INVALID_BUFFER);
        }
        assert_eq!(output, [42; 3]);
    }

    #[test]
    fn rejects_overlap_before_writing() {
        let mut timestamps = [1_u32, 2, 3, 4];
        let input = timestamps.as_mut_ptr();
        let overlap = input.wrapping_add(1);
        // SAFETY: The overlapping live ranges are rejected before pointer access.
        let status = unsafe { hermes_rust_rtp_timestamp_distances(0, input, 3, overlap, 3) };
        assert_eq!(status, INVALID_BUFFER);
        assert_eq!(timestamps, [1, 2, 3, 4]);
    }
}
