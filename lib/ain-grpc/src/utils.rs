use ethereum_types::{H160, H256, U256};

/// Revert custom error data contains:
/// Function selector (4) + offset (32) + string length (32) and the subsequent string data
const MESSAGE_LENGTH_START_IDX: usize = 36;
const MESSAGE_START_IDX: usize = 68;

pub fn format_h256(hash: H256) -> String {
    format!("{hash:#x}")
}

pub fn format_address(hash: H160) -> String {
    format!("{hash:#x}")
}

pub fn format_u256(number: U256) -> String {
    format!("{number:#x}")
}

pub fn try_get_reverted_error_or_default(data: &[u8]) -> String {
    if data.len() > MESSAGE_START_IDX {
        let message_len = U256::from(&data[MESSAGE_LENGTH_START_IDX..MESSAGE_START_IDX]);
        let Ok(message_len) = usize::try_from(message_len) else {
            return Default::default();
        };
        let Some(message_end) = MESSAGE_START_IDX.checked_add(message_len) else {
            return Default::default();
        };

        if data.len() >= message_end {
            let body: &[u8] = &data[MESSAGE_START_IDX..message_end];
            if let Ok(reason) = std::str::from_utf8(body) {
                return format!("execution reverted: {reason}");
            }
        }
    }
    // Failed to deserialize, return default
    Default::default()
}

#[cfg(test)]
mod tests {
    use ethereum_types::{H160, H256, U256};

    use super::{format_address, format_h256, format_u256, try_get_reverted_error_or_default};

    fn encode_revert_reason(reason: &[u8]) -> Vec<u8> {
        let mut data = Vec::new();
        // 4-byte selector for Error(string)
        data.extend_from_slice(&[0x08, 0xc3, 0x79, 0xa0]);
        // 32-byte offset (0x20)
        let mut offset = [0u8; 32];
        offset[31] = 0x20;
        data.extend_from_slice(&offset);
        // 32-byte string length
        let mut len_bytes = [0u8; 32];
        U256::from(reason.len()).to_big_endian(&mut len_bytes);
        data.extend_from_slice(&len_bytes);
        // string bytes
        data.extend_from_slice(reason);
        data
    }

    #[test]
    fn format_helpers_produce_hex_strings() {
        assert_eq!(
            format_h256(H256::from_low_u64_be(0xab)),
            "0x00000000000000000000000000000000000000000000000000000000000000ab"
        );
        assert_eq!(
            format_address(H160::from_low_u64_be(0xcd)),
            "0x00000000000000000000000000000000000000cd"
        );
        assert_eq!(format_u256(U256::from(255)), "0xff");
        assert_eq!(format_u256(U256::zero()), "0x0");
    }

    #[test]
    fn try_get_reverted_error_decodes_valid_reason() {
        let data = encode_revert_reason(b"insufficient balance");
        assert_eq!(
            try_get_reverted_error_or_default(&data),
            "execution reverted: insufficient balance"
        );
    }

    #[test]
    fn try_get_reverted_error_returns_empty_on_malformed_inputs() {
        // Too short (<= 68 bytes)
        assert_eq!(try_get_reverted_error_or_default(&[]), "");
        assert_eq!(try_get_reverted_error_or_default(&[0u8; 68]), "");

        // Length > usize::MAX (U256::MAX)
        let mut huge_len = vec![0u8; 100];
        huge_len[36..68].fill(0xff);
        assert_eq!(try_get_reverted_error_or_default(&huge_len), "");

        // Length causes checked_add overflow (usize::MAX)
        let mut overflow_len = vec![0u8; 100];
        let mut len_bytes = [0u8; 32];
        U256::from(usize::MAX).to_big_endian(&mut len_bytes);
        overflow_len[36..68].copy_from_slice(&len_bytes);
        assert_eq!(try_get_reverted_error_or_default(&overflow_len), "");

        // Declared length exceeds actual buffer length
        let mut truncated = encode_revert_reason(b"hello world");
        truncated.truncate(72);
        assert_eq!(try_get_reverted_error_or_default(&truncated), "");

        // Invalid UTF-8 payload
        let invalid_utf8 = encode_revert_reason(&[0xff, 0xfe, 0xfd]);
        assert_eq!(try_get_reverted_error_or_default(&invalid_utf8), "");
    }
}
