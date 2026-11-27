use rust_decimal::Decimal;
use rust_decimal_macros::dec;

use crate::api::stats::COIN;

#[derive(Debug)]
pub struct BlockRewardDistribution {
    pub masternode: Decimal,
    pub community: Decimal,
    pub anchor: Decimal,
    pub liquidity: Decimal,
    pub loan: Decimal,
    pub options: Decimal,
    pub unallocated: Decimal,
}

pub const BLOCK_REWARD_DISTRIBUTION_PERCENTAGE: BlockRewardDistribution = BlockRewardDistribution {
    masternode: dec!(3333),
    community: dec!(491),
    anchor: dec!(2),
    liquidity: dec!(2545),
    loan: dec!(2468),
    options: dec!(988),
    unallocated: dec!(173),
};

/**
 * Get block reward distribution from block base subsidy
 */
pub fn get_block_reward_distribution(subsidy: Decimal) -> BlockRewardDistribution {
    BlockRewardDistribution {
        masternode: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.masternode),
        community: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.community),
        anchor: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.anchor),
        liquidity: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.liquidity),
        loan: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.loan),
        options: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.options),
        unallocated: calculate_reward(subsidy, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.unallocated),
    }
}

fn calculate_reward(amount: Decimal, percent: Decimal) -> Decimal {
    (amount * percent) / dec!(10000) / Decimal::from(COIN)
}

#[cfg(test)]
mod tests {
    use rust_decimal_macros::dec;

    use super::{get_block_reward_distribution, BLOCK_REWARD_DISTRIBUTION_PERCENTAGE};

    #[test]
    fn test_block_reward_distribution_sums_to_full_subsidy() {
        let total_pct = BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.masternode
            + BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.community
            + BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.anchor
            + BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.liquidity
            + BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.loan
            + BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.options
            + BLOCK_REWARD_DISTRIBUTION_PERCENTAGE.unallocated;
        assert_eq!(total_pct, dec!(10000));

        // 100 DFI in satoshis
        let dist = get_block_reward_distribution(dec!(10_000_000_000));
        assert_eq!(dist.masternode, dec!(33.33));
        assert_eq!(dist.community, dec!(4.91));
        assert_eq!(dist.anchor, dec!(0.02));
        assert_eq!(dist.liquidity, dec!(25.45));
        assert_eq!(dist.loan, dec!(24.68));
        assert_eq!(dist.options, dec!(9.88));
        assert_eq!(dist.unallocated, dec!(1.73));
    }
}
