use serde::{Deserialize, Serialize};

use crate::{model::Masternode, network::Network};

#[derive(Serialize, Deserialize, Debug, Default, Clone)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum MasternodeState {
    PreEnabled,
    Enabled,
    PreResigned,
    Resigned,
    PreBanned,
    Banned,
    #[default]
    Unknown,
}

pub struct MasternodeService {
    network: Network,
}

impl MasternodeService {
    pub fn new(network: Network) -> Self {
        Self { network }
    }

    fn get_mn_activation_delay(&self, height: u32) -> u32 {
        let eunos_height = self.network.fork_heights().df8_eunos_height;
        if height < eunos_height {
            self.network.params().activation_delay
        } else {
            self.network.params().new_activation_delay
        }
    }

    fn get_mn_resign_delay(&self, height: u32) -> u32 {
        let eunos_height = self.network.fork_heights().df8_eunos_height;
        if height < eunos_height {
            self.network.params().resign_delay
        } else {
            self.network.params().new_resign_delay
        }
    }

    pub fn get_masternode_state(&self, masternode: &Masternode, height: u32) -> MasternodeState {
        if let Some(resign_height) = masternode.resign_height {
            let resign_delay = self.get_mn_resign_delay(resign_height);
            if height < resign_height + resign_delay {
                MasternodeState::PreResigned
            } else {
                MasternodeState::Resigned
            }
        } else {
            let activation_delay = self.get_mn_activation_delay(masternode.creation_height);
            if masternode.creation_height == 0
                || height >= masternode.creation_height + activation_delay
            {
                MasternodeState::Enabled
            } else {
                MasternodeState::PreEnabled
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use bitcoin::{hashes::Hash, Txid};
    use rust_decimal_macros::dec;

    use super::{MasternodeService, MasternodeState};
    use crate::{
        model::{BlockContext, Masternode},
        network::Network,
    };

    fn make_mn(creation_height: u32, resign_height: Option<u32>) -> Masternode {
        Masternode {
            id: Txid::all_zeros(),
            owner_address: "owner".to_string(),
            operator_address: "operator".to_string(),
            creation_height,
            resign_height,
            resign_tx: None,
            minted_blocks: 0,
            timelock: 0,
            collateral: dec!(20000),
            block: BlockContext::default(),
            history: vec![],
        }
    }

    #[test]
    fn test_masternode_state_transitions_pre_and_post_eunos() {
        let svc = MasternodeService::new(Network::Mainnet);

        // Genesis masternode is immediately enabled
        assert!(matches!(
            svc.get_masternode_state(&make_mn(0, None), 0),
            MasternodeState::Enabled
        ));

        // Pre-Eunos activation delay is 10
        let mn_pre = make_mn(100, None);
        assert!(matches!(
            svc.get_masternode_state(&mn_pre, 109),
            MasternodeState::PreEnabled
        ));
        assert!(matches!(
            svc.get_masternode_state(&mn_pre, 110),
            MasternodeState::Enabled
        ));

        // Post-Eunos activation delay is 1008
        let mn_post = make_mn(900_000, None);
        assert!(matches!(
            svc.get_masternode_state(&mn_post, 901_007),
            MasternodeState::PreEnabled
        ));
        assert!(matches!(
            svc.get_masternode_state(&mn_post, 901_008),
            MasternodeState::Enabled
        ));

        // Pre-Eunos resign delay is 60
        let mn_resign_pre = make_mn(100, Some(200));
        assert!(matches!(
            svc.get_masternode_state(&mn_resign_pre, 259),
            MasternodeState::PreResigned
        ));
        assert!(matches!(
            svc.get_masternode_state(&mn_resign_pre, 260),
            MasternodeState::Resigned
        ));

        // Post-Eunos resign delay is 2016
        let mn_resign_post = make_mn(900_000, Some(905_000));
        assert!(matches!(
            svc.get_masternode_state(&mn_resign_post, 907_015),
            MasternodeState::PreResigned
        ));
        assert!(matches!(
            svc.get_masternode_state(&mn_resign_post, 907_016),
            MasternodeState::Resigned
        ));
    }
}
