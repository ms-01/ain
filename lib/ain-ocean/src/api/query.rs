use axum::{
    async_trait,
    extract::FromRequestParts,
    http::{request::Parts, StatusCode},
};
use serde::{
    de::{DeserializeOwned, Deserializer},
    Deserialize,
};
use serde_with::{serde_as, DisplayFromStr};

use crate::error::ApiError;

const DEFAUT_PAGINATION_SIZE: usize = 30;

pub fn default_pagination_size() -> usize {
    DEFAUT_PAGINATION_SIZE
}

#[serde_as]
#[derive(Deserialize, Debug)]
pub struct PaginationQuery {
    #[serde_as(as = "DisplayFromStr")]
    #[serde(default = "default_pagination_size")]
    pub size: usize,
    #[serde(default)]
    #[serde(deserialize_with = "undefined_to_none")]
    pub next: Option<String>,
}

impl Default for PaginationQuery {
    fn default() -> Self {
        Self {
            size: DEFAUT_PAGINATION_SIZE,
            next: None,
        }
    }
}

fn undefined_to_none<'de, D>(d: D) -> Result<Option<String>, D::Error>
where
    D: Deserializer<'de>,
{
    let v: Option<String> = Deserialize::deserialize(d)?;
    match v {
        Some(v) if v.as_str() != "undefined" => Ok(Some(v)),
        _ => Ok(None),
    }
}

pub struct Query<T>(pub T);

#[async_trait]
impl<S, T> FromRequestParts<S> for Query<T>
where
    T: Default + DeserializeOwned,
    S: Send + Sync,
{
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, _state: &S) -> Result<Self, Self::Rejection> {
        let query = parts.uri.query().unwrap_or_default();
        if query.is_empty() {
            return Ok(Self(T::default()));
        }
        match serde_urlencoded::from_str(query) {
            Ok(v) => Ok(Self(v)),
            Err(e) => Err(ApiError::new(
                StatusCode::BAD_REQUEST,
                format!("Invalid query parameter value for {query}. {e}"),
                parts.uri.to_string(),
            )),
        }
    }
}

#[cfg(test)]
mod tests {
    use axum::{extract::FromRequestParts, http::Request};

    use super::{default_pagination_size, PaginationQuery, Query};

    #[test]
    fn test_pagination_query_defaults_and_serde() {
        assert_eq!(default_pagination_size(), 30);
        let def = PaginationQuery::default();
        assert_eq!(def.size, 30);
        assert_eq!(def.next, None);

        let q: PaginationQuery = serde_urlencoded::from_str("size=10&next=abc").unwrap();
        assert_eq!(q.size, 10);
        assert_eq!(q.next, Some("abc".to_string()));

        let q_undef: PaginationQuery =
            serde_urlencoded::from_str("size=5&next=undefined").unwrap();
        assert_eq!(q_undef.size, 5);
        assert_eq!(q_undef.next, None);
    }

    #[tokio::test]
    async fn test_query_extractor() {
        let req = Request::builder().uri("/blocks").body(()).unwrap();
        let (mut parts, _) = req.into_parts();
        let Query(q) = Query::<PaginationQuery>::from_request_parts(&mut parts, &())
            .await
            .unwrap();
        assert_eq!(q.size, 30);
        assert_eq!(q.next, None);

        let req = Request::builder()
            .uri("/blocks?size=15&next=100")
            .body(())
            .unwrap();
        let (mut parts, _) = req.into_parts();
        let Query(q) = Query::<PaginationQuery>::from_request_parts(&mut parts, &())
            .await
            .unwrap();
        assert_eq!(q.size, 15);
        assert_eq!(q.next, Some("100".to_string()));

        let req = Request::builder()
            .uri("/blocks?size=notanumber")
            .body(())
            .unwrap();
        let (mut parts, _) = req.into_parts();
        let res = Query::<PaginationQuery>::from_request_parts(&mut parts, &()).await;
        assert!(res.is_err());
    }
}
