use std::str::FromStr;

use ain_dftx::{Currency, Token};
use bitcoin::{Address, ScriptBuf, Txid};
use defichain_rpc::json::token::TokenInfo;
use rust_decimal::Decimal;
use rust_decimal_macros::dec;
use snafu::OptionExt;

use super::query::PaginationQuery;
use crate::{
    error::{
        Error::ToArrayError, InvalidAmountSnafu, InvalidFixedIntervalPriceSnafu,
        InvalidPoolPairSymbolSnafu, InvalidPriceTickerSortKeySnafu, InvalidTokenCurrencySnafu,
    },
    hex_encoder::as_sha256,
    model::PriceTickerId,
    network::Network,
    Result,
};

#[must_use]
pub fn parse_display_symbol(token_info: &TokenInfo) -> String {
    if token_info.is_lps {
        let tokens: Vec<&str> = token_info.symbol.split('-').collect();
        if tokens.len() == 2 {
            return format!(
                "{}-{}",
                parse_dat_symbol(tokens[0]),
                parse_dat_symbol(tokens[1])
            );
        }
    } else if token_info.is_dat {
        return parse_dat_symbol(&token_info.symbol);
    }

    token_info.symbol.clone()
}

#[must_use]
pub fn parse_dat_symbol(symbol: &str) -> String {
    let special_symbols = ["DUSD", "DFI", "csETH"];

    if special_symbols.contains(&symbol) {
        symbol.to_string()
    } else {
        format!("d{symbol}")
    }
}

pub fn parse_pool_pair_symbol(item: &str) -> Result<(String, String)> {
    let mut parts = item.split('-');
    let a = parts
        .next()
        .context(InvalidPoolPairSymbolSnafu { item })?
        .to_string();
    let b = parts
        .next()
        .context(InvalidPoolPairSymbolSnafu { item })?
        .to_string();

    Ok((a, b))
}

pub fn parse_token_currency(item: &str) -> Result<(Token, Currency)> {
    let mut parts = item.split('-');
    let token = parts
        .next()
        .context(InvalidTokenCurrencySnafu { item })?
        .to_string();
    let currency = parts
        .next()
        .context(InvalidTokenCurrencySnafu { item })?
        .to_string();

    Ok((token, currency))
}

pub fn parse_fixed_interval_price(item: &str) -> Result<(Token, Currency)> {
    let mut parts = item.split('/');
    let token = parts
        .next()
        .context(InvalidFixedIntervalPriceSnafu { item })?
        .to_string();
    let currency = parts
        .next()
        .context(InvalidFixedIntervalPriceSnafu { item })?
        .to_string();

    Ok((token, currency))
}

pub fn parse_amount(item: &str) -> Result<(String, String)> {
    let mut parts = item.split('@');
    let amount = parts
        .next()
        .context(InvalidAmountSnafu { item })?
        .to_string();
    let symbol = parts
        .next()
        .context(InvalidAmountSnafu { item })?
        .to_string();

    Ok((amount, symbol))
}

pub fn parse_query_height_txno(item: &str) -> Result<(u32, usize)> {
    let mut parts = item.split('-');
    let height = parts.next().context(InvalidAmountSnafu { item })?;
    let txno = parts.next().context(InvalidAmountSnafu { item })?;

    let height = height.parse::<u32>()?;
    let txno = txno.parse::<usize>()?;

    Ok((height, txno))
}

pub fn parse_query_height_txid(item: &str) -> Result<(u32, Txid)> {
    let mut parts = item.split('-');
    let encoded_height = parts.next().context(InvalidAmountSnafu { item })?;
    let txid = parts.next().context(InvalidAmountSnafu { item })?;

    let height_in_bytes: [u8; 4] = hex::decode(encoded_height)?
        .try_into()
        .map_err(|_| ToArrayError)?;
    let height = u32::from_be_bytes(height_in_bytes);
    let txid = txid.parse::<Txid>()?;

    Ok((height, txid))
}

pub fn parse_price_ticker_sort(item: &str) -> Result<PriceTickerId> {
    let mut parts = item.split('-');
    let count_height_token = parts
        .next()
        .context(InvalidPriceTickerSortKeySnafu { item })?;
    let encoded_count = &count_height_token[..8];
    let encoded_height = &count_height_token[8..16];
    let token = &count_height_token[16..];
    let token = token.to_string();

    let count: [u8; 4] = hex::decode(encoded_count)?
        .try_into()
        .map_err(|_| ToArrayError)?;

    let height: [u8; 4] = hex::decode(encoded_height)?
        .try_into()
        .map_err(|_| ToArrayError)?;

    let currency = parts
        .next()
        .context(InvalidTokenCurrencySnafu { item })?
        .to_string();

    Ok((count, height, token, currency))
}

#[must_use]
pub fn format_number(v: Decimal) -> String {
    if v == dec!(0) {
        "0".to_string()
    } else {
        format!("{v:.8}")
    }
}

pub fn from_script(script: &ScriptBuf, network: Network) -> Result<String> {
    Ok(Address::from_script(script, network.into())?.to_string())
}

#[test]
fn test_from_script() {
    // OP_0 { type: 'OP_0', code: 0 },
    // OP_PUSHDATA {
    //   type: 'OP_PUSHDATA',
    //   hex: '05768f2d17f0016b5720bb49859cbb065041716f'
    // }
    let script = ScriptBuf::from_hex("001405768f2d17f0016b5720bb49859cbb065041716f").unwrap();
    let addr = from_script(&script, Network::Mainnet).unwrap();
    assert_eq!(
        addr,
        "df1qq4mg7tgh7qqkk4eqhdyct89mqegyzut0jjz8rg".to_string()
    )
}

pub fn to_script(address: &str, network: Network) -> Result<ScriptBuf> {
    let addr = Address::from_str(address)?.require_network(network.into())?;
    Ok(ScriptBuf::from(addr))
}

pub fn address_to_hid(address: &str, network: Network) -> Result<[u8; 32]> {
    let script = to_script(address, network)?;
    let bytes = script.to_bytes();
    Ok(as_sha256(&bytes))
}

/// Finds the balance of a specified token symbol within a list of token strings.
///
/// This function iterates through a vector of token strings, where each string
/// represents an amount followed by a token symbol in the format "amount@symbol".
/// It searches for the specified token symbol and returns the corresponding balance.
/// If the token symbol is not found or if there are any parsing errors, it returns 0.
///
/// # Arguments
///
/// * `tokens` - A vector of strings representing token amounts with their symbols.
/// * `symbol` - A reference to a string representing the token symbol to find the balance for.
///
/// # Examples
///
/// ```
/// use rust_decimal::Decimal;
/// use rust_decimal_macros::dec;
/// use ain_ocean::api::{common::find_token_balance};
///
/// let tokens = vec![
///     "557.35080849@DFI".to_string(),
///     "9.98000000@BTC".to_string(),
///     "421.46947098@DUSD".to_string()
/// ];
/// let balance = find_token_balance(tokens, "DFI");
/// assert_eq!(balance, dec!(557.35080849));
/// ```
///
/// # Returns
///
/// The balance of the specified token symbol if found; otherwise, returns 0.
#[must_use]
pub fn find_token_balance(tokens: Vec<String>, symbol: &str) -> Decimal {
    tokens
        .iter()
        .find_map(|t| {
            t.ends_with(symbol)
                .then(|| t.split('@').next().and_then(|v| v.parse::<Decimal>().ok()))
                .flatten()
        })
        .unwrap_or_default()
}

/// Provides a simulated pagination mechanism for iterators where native pagination is not available.
///
/// This trait extends any Rust iterator to include a `paginate` method, allowing for pseudo-pagination
/// based on custom logic. It's should only be used to query defid list* RPC that don't implement native pagination
///
/// # Warning
///
/// This method should be used cautiously, as it involves retrieving all data from the data source
/// before applying pagination. This can lead to significant performance and resource usage issues,
/// especially with large datasets. It is recommended to use this approach only defid does not accept
/// any pagination parameter.
///
/// # Parameters
///
/// - `query`: A reference to `PaginationQuery`
/// - `skip_while`: A closure that determines the starting point of data to consider, mimicking the
///   'start' parameter in traditional pagination. Once an item fails this condition, pagination starts.
///
/// # Example
///
/// This example is illustrative only and should not be executed directly as it involves API calls.
///
/// ```rust,ignore
/// use ain_ocean::api::common::Paginate;
///
/// let query = PaginationQuery {
///     next: Some(1)
///     limit: 3
/// };
///
/// let skip_while = |el: &LoanSchemeResult| match &query.next {
///     None => false,
///     Some(v) => v != &el.id,
/// };
/// let res: Vec<_> = ctx
///     .client
///     .list_loan_schemes()
///     .await?
///     .into_iter()
///     .fake_paginate(&query, skip_while)
///     .collect();
///
/// assert!(res.len() <= query.size, "The result should not contain more items than the specified limit");
/// assert!(res[0].id > query.next.unwrap(), "The result should start after the requested start id");
/// ```
pub trait Paginate<'a, T>: Iterator<Item = T> + Sized {
    fn fake_paginate<F>(
        self,
        query: &PaginationQuery,
        skip_while: F,
    ) -> Box<dyn Iterator<Item = T> + 'a>
    where
        F: FnMut(&T) -> bool + 'a;
    fn paginate(self, query: &PaginationQuery) -> Box<dyn Iterator<Item = T> + 'a>;
}

impl<'a, T, I> Paginate<'a, T> for I
where
    I: Iterator<Item = T> + 'a,
{
    fn fake_paginate<F>(
        self,
        query: &PaginationQuery,
        skip_while: F,
    ) -> Box<dyn Iterator<Item = T> + 'a>
    where
        F: FnMut(&T) -> bool + 'a,
    {
        Box::new(
            self.skip_while(skip_while)
                .skip(usize::from(query.next.is_some()))
                .take(query.size),
        )
    }
    fn paginate(self, query: &PaginationQuery) -> Box<dyn Iterator<Item = T> + 'a> {
        Box::new(
            self.skip(usize::from(query.next.is_some()))
                .take(query.size),
        )
    }
}

#[cfg(test)]
mod tests {
    use bitcoin::{hashes::Hash, Txid};
    use defichain_rpc::json::token::TokenInfo;
    use rust_decimal_macros::dec;

    use super::{
        address_to_hid, find_token_balance, format_number, parse_amount, parse_dat_symbol,
        parse_display_symbol, parse_fixed_interval_price, parse_pool_pair_symbol,
        parse_price_ticker_sort, parse_query_height_txid, parse_query_height_txno,
        parse_token_currency, to_script, Paginate,
    };
    use crate::{api::query::PaginationQuery, network::Network};

    fn make_token_info(symbol: &str, is_dat: bool, is_lps: bool) -> TokenInfo {
        TokenInfo {
            symbol: symbol.to_string(),
            symbol_key: symbol.to_string(),
            name: symbol.to_string(),
            decimal: 8,
            limit: 0,
            mintable: true,
            tradeable: true,
            is_dat,
            is_lps,
            finalized: false,
            is_loan_token: false,
            minted: 0.0,
            creation_tx: Txid::all_zeros().to_string(),
            creation_height: 0,
            destruction_tx: Txid::all_zeros().to_string(),
            destruction_height: 0,
            collateral_address: None,
        }
    }

    #[test]
    fn test_parse_dat_and_display_symbol() {
        assert_eq!(parse_dat_symbol("DFI"), "DFI");
        assert_eq!(parse_dat_symbol("DUSD"), "DUSD");
        assert_eq!(parse_dat_symbol("csETH"), "csETH");
        assert_eq!(parse_dat_symbol("BTC"), "dBTC");

        assert_eq!(
            parse_display_symbol(&make_token_info("BTC-DFI", true, true)),
            "dBTC-DFI"
        );
        assert_eq!(
            parse_display_symbol(&make_token_info("BTC", true, false)),
            "dBTC"
        );
        assert_eq!(
            parse_display_symbol(&make_token_info("CUSTOM", false, false)),
            "CUSTOM"
        );
        assert_eq!(
            parse_display_symbol(&make_token_info("SINGLE", true, true)),
            "SINGLE"
        );
    }

    #[test]
    fn test_string_parsers() {
        assert_eq!(
            parse_pool_pair_symbol("BTC-DFI").unwrap(),
            ("BTC".to_string(), "DFI".to_string())
        );
        assert!(parse_pool_pair_symbol("BTCDFI").is_err());

        assert_eq!(
            parse_token_currency("BTC-USD").unwrap(),
            ("BTC".to_string(), "USD".to_string())
        );
        assert!(parse_token_currency("BTCUSD").is_err());

        assert_eq!(
            parse_fixed_interval_price("BTC/USD").unwrap(),
            ("BTC".to_string(), "USD".to_string())
        );
        assert!(parse_fixed_interval_price("BTC-USD").is_err());

        assert_eq!(
            parse_amount("12.5@DFI").unwrap(),
            ("12.5".to_string(), "DFI".to_string())
        );
        assert!(parse_amount("12.5DFI").is_err());

        assert_eq!(parse_query_height_txno("100-5").unwrap(), (100, 5));
        assert!(parse_query_height_txno("100").is_err());
        assert!(parse_query_height_txno("abc-5").is_err());

        let txid = Txid::all_zeros();
        let height_hex = hex::encode(42u32.to_be_bytes());
        assert_eq!(
            parse_query_height_txid(&format!("{height_hex}-{txid}")).unwrap(),
            (42, txid)
        );
        assert!(parse_query_height_txid("invalid").is_err());
        assert!(parse_query_height_txid(&format!("00-{txid}")).is_err());

        let count_hex = hex::encode(3u32.to_be_bytes());
        let h_hex = hex::encode(99u32.to_be_bytes());
        let parsed = parse_price_ticker_sort(&format!("{count_hex}{h_hex}BTC-USD")).unwrap();
        assert_eq!(
            parsed,
            (
                3u32.to_be_bytes(),
                99u32.to_be_bytes(),
                "BTC".to_string(),
                "USD".to_string()
            )
        );
    }

    #[test]
    fn test_format_number_and_address_helpers() {
        assert_eq!(format_number(dec!(0)), "0");
        assert_eq!(format_number(dec!(1.5)), "1.50000000");

        let addr = "df1qq4mg7tgh7qqkk4eqhdyct89mqegyzut0jjz8rg";
        let script = to_script(addr, Network::Mainnet).unwrap();
        assert_eq!(
            hex::encode(script.to_bytes()),
            "001405768f2d17f0016b5720bb49859cbb065041716f"
        );
        let hid = address_to_hid(addr, Network::Mainnet).unwrap();
        assert_eq!(hid.len(), 32);
        assert!(to_script("not-an-address", Network::Mainnet).is_err());
    }

    #[test]
    fn test_find_token_balance() {
        let tokens = vec![
            "557.35080849@DFI".to_string(),
            "9.98000000@BTC".to_string(),
            "bad@ETH".to_string(),
        ];
        assert_eq!(find_token_balance(tokens.clone(), "DFI"), dec!(557.35080849));
        assert_eq!(find_token_balance(tokens.clone(), "BTC"), dec!(9.98000000));
        assert_eq!(find_token_balance(tokens.clone(), "ETH"), dec!(0));
        assert_eq!(find_token_balance(tokens, "MISSING"), dec!(0));
    }

    #[test]
    fn test_paginate_and_fake_paginate() {
        let items = vec![1, 2, 3, 4, 5];

        let q1 = PaginationQuery {
            size: 2,
            next: None,
        };
        let page1: Vec<_> = items.clone().into_iter().paginate(&q1).collect();
        assert_eq!(page1, vec![1, 2]);

        let q2 = PaginationQuery {
            size: 2,
            next: Some("1".to_string()),
        };
        let page2: Vec<_> = items.clone().into_iter().paginate(&q2).collect();
        assert_eq!(page2, vec![2, 3]);

        let q3 = PaginationQuery {
            size: 2,
            next: Some("2".to_string()),
        };
        let fake: Vec<_> = items
            .into_iter()
            .fake_paginate(&q3, |el| *el != 2)
            .collect();
        assert_eq!(fake, vec![3, 4]);
    }
}
