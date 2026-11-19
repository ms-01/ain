use anyhow::format_err;
use ethereum_types::U256;

use crate::{EVMError, Result};

pub struct WeiAmount(pub U256);

const WEI_TO_GWEI: U256 = U256([1_000_000_000, 0, 0, 0]);
// 1 satoshi = 1e-8 DFI = 10 gwei = 1e10 wei.
// Derived from the C++ side: `WEI_IN_GWEI = 1000000000` and `CAMOUNT_TO_GWEI = 10`
// (see `src/amount.h`), i.e. 1e9 * 10 = 1e10 wei per satoshi.
const WEI_TO_SATS: U256 = U256([10_000_000_000, 0, 0, 0]);
// Maximum money amount expressed in satoshi.
// Must stay in sync with the C++ `MAX_MONEY = 1200000000 * COIN` (`COIN = 100000000`)
// from `src/amount.h`, i.e. the same 1.2e17 satoshi (= 1.2e27 wei).
pub const MAX_MONEY_SATS: U256 = U256([120_000_000_000_000_000, 0, 0, 0]);

impl WeiAmount {
    pub fn to_gwei(&self) -> Result<U256> {
        if !self.wei_range() {
            return Err(EVMError::MoneyRangeError(
                "value more than money range".to_string(),
            ));
        }
        Ok(self.0 / WEI_TO_GWEI)
    }

    pub fn to_satoshi(&self) -> Result<U256> {
        if !self.wei_range() {
            return Err(EVMError::MoneyRangeError(
                "value more than money range".to_string(),
            ));
        }
        Ok(self.0 / WEI_TO_SATS)
    }

    pub fn wei_range(&self) -> bool {
        let max_money_sats = MAX_MONEY_SATS;
        let max_money_wei = max_money_sats.saturating_mul(WEI_TO_SATS);
        self.0 <= max_money_wei
    }
}

pub fn try_from_gwei(gwei: U256) -> Result<WeiAmount> {
    let wei = gwei.checked_mul(WEI_TO_GWEI);
    match wei {
        Some(wei) => Ok(WeiAmount(wei)),
        None => Err(format_err!("convert gwei to wei failed from overflow").into()),
    }
}

pub fn try_from_satoshi(satoshi: U256) -> Result<WeiAmount> {
    let wei = satoshi.checked_mul(WEI_TO_SATS);
    match wei {
        Some(wei) => Ok(WeiAmount(wei)),
        None => Err(format_err!("convert gwei to wei failed from overflow").into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The maximum money amount in wei: `MAX_MONEY_SATS * WEI_TO_SATS`.
    fn max_money_wei() -> U256 {
        MAX_MONEY_SATS.saturating_mul(WEI_TO_SATS)
    }

    #[test]
    fn test_wei_to_gwei_and_sats_constants() {
        assert_eq!(WEI_TO_GWEI, U256::from(1_000_000_000u64));
        assert_eq!(WEI_TO_SATS, U256::from(10_000_000_000u64));
        // 1 satoshi = 10 gwei, hence 1e10 wei per satoshi.
        assert_eq!(WEI_TO_SATS, WEI_TO_GWEI * U256::from(10u64));
    }

    #[test]
    fn test_max_money_sats_matches_cpp_max_money() {
        // C++: `static const CAmount MAX_MONEY = 1200000000 * COIN;` with
        // `static constexpr CAmount COIN = 100000000;` (src/amount.h).
        let cpp_max_money = 1_200_000_000u64 * 100_000_000u64;
        assert_eq!(MAX_MONEY_SATS, U256::from(cpp_max_money));
        assert_eq!(MAX_MONEY_SATS, U256::from(120_000_000_000_000_000u64));
        // The value must still fit into a u64 (as the C++ `CAmount` is an int64_t).
        assert!(MAX_MONEY_SATS <= U256::from(u64::MAX));
    }

    #[test]
    fn test_wei_range_bound_matches_max_money_sats() {
        assert_eq!(
            max_money_wei(),
            U256::from(120_000_000_000_000_000u64) * WEI_TO_SATS
        );
        // Absolute value: 1.2e17 satoshi * 1e10 wei = 1.2e27 wei.
        assert_eq!(
            max_money_wei(),
            U256::from(1_200_000_000_000_000_000_000_000_000u128)
        );
        assert_eq!(max_money_wei() / WEI_TO_SATS, MAX_MONEY_SATS);
    }

    #[test]
    fn test_wei_range_boundary() {
        assert!(WeiAmount(U256::zero()).wei_range());
        assert!(WeiAmount(max_money_wei()).wei_range());
        assert!(!WeiAmount(max_money_wei() + U256::one()).wei_range());
        assert!(!WeiAmount(U256::MAX).wei_range());
    }

    #[test]
    fn test_try_from_satoshi_roundtrip() {
        for satoshi in [
            U256::zero(),
            U256::from(1u64),
            U256::from(100_000_000u64), // 1 DFI
            U256::from(1_000_000u64),
            MAX_MONEY_SATS,
        ] {
            let amount = try_from_satoshi(satoshi).unwrap();
            assert_eq!(amount.to_satoshi().unwrap(), satoshi);
        }
    }

    #[test]
    fn test_try_from_gwei_roundtrip() {
        for gwei in [
            U256::zero(),
            U256::from(1u64),
            U256::from(1_000_000_000u64),
            max_money_wei() / WEI_TO_GWEI,
        ] {
            let amount = try_from_gwei(gwei).unwrap();
            assert_eq!(amount.to_gwei().unwrap(), gwei);
        }
    }

    #[test]
    fn test_to_satoshi_truncates() {
        // 1.5 gwei -> 1 gwei and 0 satoshi (truncated, never rounded).
        let amount = WeiAmount(U256::from(1_500_000_000u64));
        assert_eq!(amount.to_gwei().unwrap(), U256::from(1u64));
        assert_eq!(amount.to_satoshi().unwrap(), U256::zero());

        // 19.999999999 gwei -> 19 gwei and 1 satoshi.
        let amount = WeiAmount(U256::from(19_999_999_999u64));
        assert_eq!(amount.to_gwei().unwrap(), U256::from(19u64));
        assert_eq!(amount.to_satoshi().unwrap(), U256::from(1u64));
    }

    #[test]
    fn test_max_money_sats_boundary() {
        // Exactly `MAX_MONEY_SATS` satoshi is inside the money range.
        let amount = try_from_satoshi(MAX_MONEY_SATS).unwrap();
        assert!(amount.wei_range());
        assert_eq!(amount.to_satoshi().unwrap(), MAX_MONEY_SATS);
        assert_eq!(
            amount.to_gwei().unwrap(),
            MAX_MONEY_SATS * U256::from(10u64)
        );

        // One satoshi more is outside the money range.
        let amount = try_from_satoshi(MAX_MONEY_SATS + U256::one()).unwrap();
        assert!(!amount.wei_range());
        assert!(matches!(
            amount.to_satoshi(),
            Err(EVMError::MoneyRangeError(_))
        ));
        assert!(matches!(
            amount.to_gwei(),
            Err(EVMError::MoneyRangeError(_))
        ));

        // The same boundary expressed in gwei.
        let max_gwei = max_money_wei() / WEI_TO_GWEI;
        let amount = try_from_gwei(max_gwei).unwrap();
        assert!(amount.wei_range());
        assert_eq!(amount.to_gwei().unwrap(), max_gwei);

        let amount = try_from_gwei(max_gwei + U256::one()).unwrap();
        assert!(!amount.wei_range());
        assert!(matches!(
            amount.to_satoshi(),
            Err(EVMError::MoneyRangeError(_))
        ));
        assert!(matches!(
            amount.to_gwei(),
            Err(EVMError::MoneyRangeError(_))
        ));
    }

    #[test]
    fn test_out_of_range_conversions_error() {
        for amount in [
            WeiAmount(max_money_wei() + U256::one()),
            WeiAmount(U256::MAX),
        ] {
            assert!(!amount.wei_range());
            assert!(matches!(
                amount.to_gwei(),
                Err(EVMError::MoneyRangeError(_))
            ));
            assert!(matches!(
                amount.to_satoshi(),
                Err(EVMError::MoneyRangeError(_))
            ));
        }
    }

    #[test]
    fn test_try_from_satoshi_overflow() {
        assert!(matches!(
            try_from_satoshi(U256::MAX),
            Err(EVMError::Other(_))
        ));
    }

    #[test]
    fn test_try_from_gwei_overflow() {
        assert!(matches!(try_from_gwei(U256::MAX), Err(EVMError::Other(_))));
    }

    #[test]
    fn test_try_from_satoshi_does_not_enforce_money_range() {
        // The constructors only guard against overflow; the money range is enforced
        // later on by `to_gwei()` / `to_satoshi()`.
        let amount = try_from_satoshi(MAX_MONEY_SATS + U256::one()).unwrap();
        assert!(!amount.wei_range());
        assert!(matches!(
            amount.to_satoshi(),
            Err(EVMError::MoneyRangeError(_))
        ));
    }
}
