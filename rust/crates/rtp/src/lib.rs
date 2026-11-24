//! Portable arithmetic for 32-bit RTP timestamps.

/// Returns the forward distance in ticks, modulo 2³².
///
/// The caller determines the temporal order of the timestamps.
#[must_use]
pub const fn timestamp_distance(earlier: u32, later: u32) -> u32 {
    later.wrapping_sub(earlier)
}

/// Produces the forward distance from one timestamp to every timestamp in a slice.
pub fn timestamp_distances(earlier: u32, later: &[u32]) -> impl ExactSizeIterator<Item = u32> {
    later
        .iter()
        .map(move |&timestamp| timestamp_distance(earlier, timestamp))
}

#[cfg(test)]
mod tests {
    use super::{timestamp_distance, timestamp_distances};

    #[test]
    fn measures_forward_distance() {
        assert_eq!(timestamp_distance(100, 145), 45);
        assert_eq!(timestamp_distance(100, 100), 0);
        assert_eq!(timestamp_distance(u32::MAX - 1, 1), 3);
    }

    #[test]
    fn measures_each_timestamp_from_the_same_start() {
        let timestamps = [u32::MAX - 1, u32::MAX, 0, 1, 0x7fff_fffe];
        assert!(timestamp_distances(u32::MAX - 1, &timestamps).eq([0, 1, 2, 3, 0x8000_0000]));
    }
}
