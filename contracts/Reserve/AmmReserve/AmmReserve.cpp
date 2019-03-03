#include "./AmmReserve.hpp"
#include "liquidity.hpp"

using namespace eosio;

ACTION AmmReserve::init(name    owner,
                        name    network_contract,
                        symbol  token_symbol,
                        name    token_contract,
                        name    eos_contract,
                        bool    enable_trade) {
    eosio_assert(is_account(owner), "owner account does not exist");
    eosio_assert(is_account(network_contract), "network account does not exist");
    eosio_assert(is_account(token_contract), "token account does not exist");
    eosio_assert(is_account(eos_contract), "eos contract account does not exist");

    require_auth(_self);

    state_type state_inst(_self, _self.value);
    eosio_assert(!state_inst.exists(), "init already called");

    state_t new_state;
    new_state.owner = owner;
    new_state.network_contract = network_contract;
    new_state.token_symbol = token_symbol;
    new_state.token_contract = token_contract;
    new_state.eos_contract = eos_contract;
    new_state.trade_enabled = enable_trade;
    new_state.collected_fees_in_tokens.symbol = token_symbol;
    new_state.collected_fees_in_tokens.amount = 0;
    state_inst.set(new_state, _self);
}

ACTION AmmReserve::setparams(double r,
                             double p_min,
                             asset  max_eos_cap_buy,
                             asset  max_eos_cap_sell,
                             double fee_percent,
                             double max_sell_rate,
                             double min_sell_rate) {
    state_type state_inst(_self, _self.value);
    eosio_assert(state_inst.exists(), "init not called yet");
    require_auth(state_inst.get().owner);

    eosio_assert(fee_percent < 100, "illegal fee_percent");
    eosio_assert(min_sell_rate < max_sell_rate, "min_sell_rate not smaller than max_sell_rate");

    params_type params_inst(_self, _self.value);
    params_t new_params;
    new_params.r = r;
    new_params.p_min = p_min;
    new_params.max_eos_cap_buy = max_eos_cap_buy;
    new_params.max_eos_cap_sell = max_eos_cap_sell;
    new_params.fee_percent = fee_percent;
    new_params.max_buy_rate = 1.0 / min_sell_rate;
    new_params.min_buy_rate = 1.0 / max_sell_rate;
    new_params.max_sell_rate = max_sell_rate;
    new_params.min_sell_rate = min_sell_rate;
    params_inst.set(new_params, _self);
}

ACTION AmmReserve::setowner(name new_owner) {
    eosio_assert(is_account(new_owner), "new owner account does not exist");

    state_type state_inst(_self, _self.value);
    eosio_assert(state_inst.exists(), "init not called yet");

    auto s = state_inst.get();
    require_auth(s.owner);

    s.owner = new_owner;
    state_inst.set(s, _self);
}

ACTION AmmReserve::setnetwork(name network_contract) {
    eosio_assert(is_account(network_contract), "network account does not exist");

    state_type state_inst(_self, _self.value);
    eosio_assert(state_inst.exists(), "init not called yet");

    auto s = state_inst.get();
    require_auth(s.owner);

    s.network_contract = network_contract;
    state_inst.set(s, _self);
}

ACTION AmmReserve::setenable(bool enable) {
    state_type state_inst(_self, _self.value);
    eosio_assert(state_inst.exists(), "init not called yet");

    auto s = state_inst.get();
    require_auth(s.owner);

    s.trade_enabled = enable;
    state_inst.set(s, _self);
}

ACTION AmmReserve::resetfee() {
    state_type state_inst(_self, _self.value);
    eosio_assert(state_inst.exists(), "init not called yet");

    auto s = state_inst.get();
    require_auth(s.owner);

    s.collected_fees_in_tokens.amount = 0;
    state_inst.set(s, _self);
}

ACTION AmmReserve::getconvrate(asset src) {
    double rate;
    asset dest;

    rate = reserve_get_conv_rate(src, dest);
    if (!rate) dest = asset();

    rate_type rate_inst(_self, _self.value);
    rate_t s = {rate, dest};
    rate_inst.set(s, _self);
}

ACTION AmmReserve::withdraw(name to, asset quantity, name dest_contract) {
    eosio_assert(is_account(to), "to account does not exist");
    eosio_assert(is_account(dest_contract), "dest contract account does not exist");

    state_type state_inst(_self, _self.value);
    eosio_assert(state_inst.exists(), "init not called yet");
    require_auth(state_inst.get().owner);

    trans(_self, to, quantity, dest_contract, "");
}

double AmmReserve::reserve_get_conv_rate(asset src, asset &dest) {

    state_type state_inst(_self, _self.value);
    /* if reserve not ready return gracefully to continue queries in network */
    if (!state_inst.exists()) return 0;
    auto current_state = state_inst.get();
    if (!current_state.trade_enabled) return 0;

    /* verify params were set */
    params_type params_inst(_self, _self.value);
    if (!params_inst.exists()) return 0;
    auto params = params_inst.get();

    bool buy = (EOS_SYMBOL == src.symbol) ? true : false;
    double rate = liquidity_get_rate(_self, current_state.eos_contract, (liquidity_params*)(&params), buy, src);
    if (rate == 0) return 0;

    symbol dest_symbol = buy ? current_state.token_symbol : EOS_SYMBOL;
    int64_t dest_amount = calc_dest_amount(rate, src.symbol.precision(), src.amount, dest_symbol.precision());
    dest = asset(dest_amount, dest_symbol);

    /* make sure reserve has enough of the dest token */
    name dest_contract = buy ? current_state.token_contract : current_state.eos_contract;
    asset this_balance = get_balance(_self, dest_contract, dest_symbol);
    if (this_balance < dest) return 0;

    return rate;
}

void AmmReserve::trade(name from, asset src, string memo, name code, state_t &current_state) {

    eosio_assert(current_state.trade_enabled, "trade disabled");
    eosio_assert(from == current_state.network_contract, "only network can perform a trade");
    eosio_assert(code == current_state.token_contract || code == current_state.eos_contract,
                 "must come from token contract or eos contract");

    eosio_assert(src.is_valid(), "invalid transfer");
    eosio_assert(src.symbol == EOS_SYMBOL || src.symbol == current_state.token_symbol,
                 "unrecognized transfer asset symbol");

    params_type params_inst(_self, _self.value);
    eosio_assert(params_inst.exists(), "params were not set");
    auto params = params_inst.get();

    eosio_assert(memo.length() > 0 , "needs a memo");
    name receiver = name(memo.c_str());
    eosio_assert(receiver != _self, "receiver can not be current contract");

    symbol dest_symbol;
    name dest_contract;
    if (src.symbol == EOS_SYMBOL) {
        dest_symbol = current_state.token_symbol;
        dest_contract = current_state.token_contract;
    } else {
        dest_symbol = EOS_SYMBOL;
        dest_contract = current_state.eos_contract;
    }

    /* get conversion rate, assuming it is stored here since getconvrate was called beforehand in this tx */
    rate_type rate_inst(_self, _self.value);
    double conversion_rate = rate_inst.get().stored_rate;
    eosio_assert(conversion_rate > 0, "conversion rate must be bigger than 0");

    int64_t dest_amount = calc_dest_amount(conversion_rate,
                                           src.symbol.precision(),
                                           src.amount,
                                           dest_symbol.precision());
    eosio_assert(dest_amount > 0, "internal error. calculated dest amount must be > 0");

    asset dest = asset(dest_amount, dest_symbol);
    bool buy = (src.symbol == EOS_SYMBOL) ? true : false;
    asset token = buy ? dest : src;
    record_fees(params, token, buy);

    /* do trade */
    trans(_self, receiver, dest, dest_contract, "");
}

void AmmReserve::record_fees(const struct params_t &params, asset token, bool buy) {
    /* require(val <= MAX_QTY); */

    double token_damount = amount_to_damount(token.amount, token.symbol.precision());
    double dfee = buy ? (token_damount * params.fee_percent / (100.0 - params.fee_percent)) :
                        (token_damount * params.fee_percent) / 100.0;
    int64_t fee_amount = damount_to_amount(dfee, token.symbol.precision());
    asset fee = asset(fee_amount, token.symbol);

    state_type state_inst(_self, _self.value);
    auto s = state_inst.get();
    s.collected_fees_in_tokens += fee;
    state_inst.set(s, _self);
}

void AmmReserve::transfer(name from, name to, asset quantity, string memo) {

    /* allow this contract to send funds (by code) and withdraw funds (by owner or self).
    * after self renounces its authorities only owner can withdraw. */
    if (to != _self) return;

    state_type state_inst(_self, _self.value);
    if (!state_inst.exists()) {
        /* if init not called yet don't trade, instead allow anyone to deposit. */
        return;
    }

    auto current_state = state_inst.get();
    if (from == current_state.owner) {
        /* owner can deposit funds, but not trade */
        return;
    } else {
        trade(from, quantity, memo, _code, current_state);
        return;
    }
    eosio_assert(false, "unreachable code");
}

extern "C" {
    [[noreturn]] void apply(uint64_t receiver, uint64_t code, uint64_t action) {
        if (action == "transfer"_n.value && code != receiver) {
            eosio::execute_action(eosio::name(receiver), eosio::name(code), &AmmReserve::transfer);
        }
        else if (code == receiver) {
            switch (action) {
                EOSIO_DISPATCH_HELPER(AmmReserve, (init)(setparams)(setowner)(setnetwork)(setenable)
                                                  (resetfee)(getconvrate)(withdraw))
            }
        }
        eosio_exit(0);
    }
}
