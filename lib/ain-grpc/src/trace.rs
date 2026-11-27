use ain_evm::trace::types::single::{TraceType, TracerInput};
use jsonrpsee::core::RpcResult;
use serde::{Deserialize, Serialize};

use crate::errors::RPCError;

#[derive(Clone, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
#[serde(rename_all = "camelCase")]
pub struct TraceParams {
    pub disable_storage: Option<bool>,
    pub disable_memory: Option<bool>,
    pub disable_stack: Option<bool>,
    /// Built-in tracer (callTracer) or Javascript expression (blockscout tracer).
    pub tracer: Option<String>,
    pub timeout: Option<String>,
}

pub fn handle_trace_params(params: Option<TraceParams>) -> RpcResult<(TracerInput, TraceType)> {
    // Set trace input and type
    match params {
        Some(TraceParams {
            tracer: Some(tracer),
            ..
        }) => {
            const BLOCKSCOUT_JS_CODE_HASH: [u8; 16] =
                hex_literal::hex!("94d9f08796f91eb13a2e82a6066882f7");
            const BLOCKSCOUT_JS_CODE_HASH_V2: [u8; 16] =
                hex_literal::hex!("89db13694675692951673a1e6e18ff02");
            let hash = sp_io::hashing::twox_128(tracer.as_bytes());
            let tracer = if hash == BLOCKSCOUT_JS_CODE_HASH || hash == BLOCKSCOUT_JS_CODE_HASH_V2 {
                Some(TracerInput::Blockscout)
            } else if tracer == "callTracer" {
                Some(TracerInput::CallTracer)
            } else {
                None
            };
            if let Some(tracer) = tracer {
                Ok((tracer, TraceType::CallList))
            } else {
                Err(RPCError::TracingParamError(hash).into())
            }
        }
        Some(params) => Ok((
            TracerInput::None,
            TraceType::Raw {
                disable_storage: params.disable_storage.unwrap_or(false),
                disable_memory: params.disable_memory.unwrap_or(false),
                disable_stack: params.disable_stack.unwrap_or(false),
            },
        )),
        _ => Ok((
            TracerInput::None,
            TraceType::Raw {
                disable_storage: false,
                disable_memory: false,
                disable_stack: false,
            },
        )),
    }
}

#[cfg(test)]
mod tests {
    use ain_evm::trace::types::single::{TraceType, TracerInput};
    use serde_json::json;

    use super::{handle_trace_params, TraceParams};

    #[test]
    fn trace_params_deserializes_camel_case_and_rejects_unknown_fields() {
        let params: TraceParams = serde_json::from_value(json!({
            "disableStorage": true,
            "disableMemory": false,
            "disableStack": true,
            "tracer": "callTracer",
            "timeout": "5s"
        }))
        .unwrap();
        assert_eq!(params.disable_storage, Some(true));
        assert_eq!(params.disable_memory, Some(false));
        assert_eq!(params.disable_stack, Some(true));
        assert_eq!(params.tracer.as_deref(), Some("callTracer"));
        assert_eq!(params.timeout.as_deref(), Some("5s"));

        assert!(serde_json::from_value::<TraceParams>(json!({ "unknown": 1 })).is_err());
    }

    #[test]
    fn handle_trace_params_handles_none_raw_call_tracer_and_invalid_js() {
        // None -> default Raw
        let (input, trace_type) = handle_trace_params(None).unwrap();
        assert!(matches!(input, TracerInput::None));
        assert!(matches!(
            trace_type,
            TraceType::Raw {
                disable_storage: false,
                disable_memory: false,
                disable_stack: false
            }
        ));

        // Raw with custom flags
        let (input, trace_type) = handle_trace_params(Some(TraceParams {
            disable_storage: Some(true),
            disable_memory: Some(true),
            disable_stack: None,
            tracer: None,
            timeout: None,
        }))
        .unwrap();
        assert!(matches!(input, TracerInput::None));
        assert!(matches!(
            trace_type,
            TraceType::Raw {
                disable_storage: true,
                disable_memory: true,
                disable_stack: false
            }
        ));

        // Built-in callTracer
        let (input, trace_type) = handle_trace_params(Some(TraceParams {
            tracer: Some("callTracer".to_string()),
            ..Default::default()
        }))
        .unwrap();
        assert!(matches!(input, TracerInput::CallTracer));
        assert!(matches!(trace_type, TraceType::CallList));

        // Unsupported JS tracer returns TracingParamError
        let err = handle_trace_params(Some(TraceParams {
            tracer: Some("{ step: function() {} }".to_string()),
            ..Default::default()
        }))
        .unwrap_err();
        assert!(err
            .message()
            .contains("javascript based tracing is not available"));
    }
}
