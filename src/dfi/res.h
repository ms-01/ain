#ifndef DEFI_DFI_RES_H
#define DEFI_DFI_RES_H

#include <tinyformat.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

enum CustomTxErrCodes : uint32_t {
    NotSpecified = 0,
    //    NotCustomTx  = 1,
    NotEnoughBalance = 1024,
    // The sum of the transfer amounts overflows. This error is raised in the
    // consensus check of the transfer sum (SumTransfers) and marks the error
    // as caused by the height dependent overflow rule. It is used by the
    // mempool to drop transactions that were accepted below the activation
    // height and are not valid anymore above it.
    TransferSumOverflow = 2048,
    // A DFI account balance would exceed MAX_MONEY (height >= nAccountMaxMoneyHeight).
    AccountBalanceOutOfRange = 4096,
    Fatal = uint32_t(1) << 31  // not allowed to fail
};

struct Res {
    bool ok{false};
    std::string msg;
    uint32_t code{0};
    std::string dbgMsg;  // CValidationState support

    // C++20 removed the aggregate initialisation rules that allowed `Res` to be
    // default constructed via `Res res{};` while the default constructor was
    // deleted. An explicit default constructor keeps the very same observable
    // state (ok == false, empty msg/dbgMsg and code == 0) and an explicit
    // four argument constructor keeps the `Res{ok, msg, code, dbgMsg}` call
    // sites working.
    Res() = default;

    Res(bool ok_, std::string msg_, uint32_t code_, std::string dbgMsg_)
        : ok{ok_},
          msg{std::move(msg_)},
          code{code_},
          dbgMsg{std::move(dbgMsg_)} {}

    operator bool() const { return ok; }

    template <typename... Args>
    static Res Err(const std::string &err, const Args &...args) {
        return Res{false, tfm::format(err, args...), 0, {}};
    }

    template <typename... Args>
    static Res ErrCode(uint32_t code, const std::string &err, const Args &...args) {
        return Res{false, tfm::format(err, args...), code, {}};
    }

    // extended version just for CValidationState support
    template <typename... Args>
    static Res ErrDbg(const std::string &debugMsg, const std::string &err, const Args &...args) {
        return {false, tfm::format(err, args...), 0, debugMsg};
    }

    template <typename... Args>
    static Res Ok(const std::string &msg, const Args &...args) {
        return Res{true, tfm::format(msg, args...), 0, {}};
    }

    template <typename T, size_t... I>
    static Res Err(const T &t, std::index_sequence<I...>) {
        using arg0 = std::tuple_element_t<0, std::decay_t<decltype(t)>>;
        if constexpr (std::is_convertible_v<arg0, uint32_t>) {
            return Res::ErrCode(std::get<I>(t)...);
        } else {
            return Res::Err(std::get<I>(t)...);
        }
    }

    static Res Ok() { return Res{true, {}, 0, {}}; }
};

template <typename T>
struct ResVal : public Res {
    std::optional<T> val{};

    ResVal() = delete;

    ResVal(const Res &errRes)
        : Res(errRes) {
        assert(!this->ok);  // if value is not provided, then it's always an error
    }
    ResVal(T value, const Res &okRes)
        : Res(okRes),
          val(std::move(value)) {
        assert(this->ok);  // if value if provided, then it's never an error
    }

    explicit operator bool() const { return ok; }

    explicit operator T() const {
        assert(ok);
        return *val;
    }

    const T &operator*() const {
        assert(ok);
        return *val;
    }

    const T *operator->() const {
        assert(ok);
        return &(*val);
    }

    T &operator*() {
        assert(ok);
        return *val;
    }

    template <typename F>
    T ValOrException(F &&func) const {
        if (!ok) {
            throw func(code, msg);
        }
        return *val;
    }

    T ValOrDefault(T default_) const {
        if (!ok) {
            return std::move(default_);
        }
        return *val;
    }
};

#endif  // DEFI_DFI_RES_H
