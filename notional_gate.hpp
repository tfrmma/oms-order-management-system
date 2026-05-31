#pragma once

// notional confirmation gate.
//
// before any order hits the SOR, this computes the USD value at stake and
// either auto-approves (below threshold) or blocks and asks for explicit
// confirmation. blocking call — intentional. if the notional is large enough
// to need a confirmation, a few milliseconds of human latency is irrelevant.
//
// in production you'd wire this to a dedicated confirmation terminal or a
// hardware button. here it's stdin, which is good enough for the prototype.

#include "oms_types.hpp"
#include "logger.hpp"
#include <cstdio>
#include <cstring>

namespace oms {

struct NotionalGateConfig {
    // orders below this threshold are auto-approved. above: explicit confirmation.
    double auto_approve_usd{50'000.0};
    // hard block regardless of confirmation. set to 0 to disable.
    double hard_block_usd{500'000.0};
};

class NotionalConfirmationGate {
public:
    explicit NotionalConfirmationGate(const NotionalGateConfig& cfg,
                                      double lot_size = 0.001) noexcept
        : cfg_(cfg), lot_size_(lot_size)
    {}

    // returns true if the order is approved to proceed.
    // blocks on confirmation if notional > auto_approve_usd.
    [[nodiscard]] bool approve(const StrategyOrderSignal& sig) noexcept {
        if (sig.limit_price_usd <= 0.0) {
            // market order — no notional cap possible, log and pass
            g_log.warn("NOTIONAL_GATE  market_order  instr=%u  qty_lots=%ld  NO_LIMIT",
                       (unsigned)sig.instr_id, (long)sig.qty_lots);
            return true;
        }

        const double notional = static_cast<double>(sig.qty_lots) * lot_size_ * sig.limit_price_usd;
        const char*  dir_str  = (sig.dir == sor::OrderDir::BUY) ? "BUY" : "SELL";

        if (cfg_.hard_block_usd > 0.0 && notional > cfg_.hard_block_usd) {
            g_log.error("NOTIONAL_GATE  HARD_BLOCK  dir=%s  notional=%.2f USD  limit=%.2f USD",
                        dir_str, notional, cfg_.hard_block_usd);
            print_block(dir_str, notional, sig);
            return false;
        }

        if (notional <= cfg_.auto_approve_usd) {
            g_log.debug("NOTIONAL_GATE  AUTO_APPROVED  dir=%s  notional=%.2f USD",
                        dir_str, notional);
            return true;
        }

        // above threshold — need explicit confirmation
        return prompt_confirmation(dir_str, notional, sig);
    }

    void update_config(const NotionalGateConfig& cfg) noexcept { cfg_ = cfg; }

private:
    [[nodiscard]] bool prompt_confirmation(const char*                dir_str,
                                           double                     notional,
                                           const StrategyOrderSignal& sig) noexcept {
        const double qty = static_cast<double>(sig.qty_lots) * lot_size_;

        std::fprintf(stderr,
            "\n"
            "  ╔══════════════════════════════════════════════════╗\n"
            "  ║           NOTIONAL CONFIRMATION REQUIRED         ║\n"
            "  ╠══════════════════════════════════════════════════╣\n"
            "  ║  Direction   : %-34s║\n"
            "  ║  Quantity    : %-8.4f BTC                       ║\n"
            "  ║  Limit price : %-10.2f USD                     ║\n"
            "  ║  Notional    : %-12.2f USD  ◄ CAPITAL AT RISK  ║\n"
            "  ║  Instrument  : %-34u║\n"
            "  ╠══════════════════════════════════════════════════╣\n"
            "  ║  Approve? [yes/no] :                             ║\n"
            "  ╚══════════════════════════════════════════════════╝\n"
            "  > ",
            dir_str,
            qty,
            sig.limit_price_usd,
            notional,
            (unsigned)sig.instr_id
        );
        std::fflush(stderr);

        char buf[8]{};
        if (!std::fgets(buf, sizeof(buf), stdin)) {
            g_log.error("NOTIONAL_GATE  CONFIRMATION_READ_FAILED  notional=%.2f", notional);
            return false;
        }

        // strip newline
        buf[std::strcspn(buf, "\n")] = '\0';

        const bool approved = (std::strncmp(buf, "yes", 3) == 0);
        g_log.info("NOTIONAL_GATE  %s  dir=%s  notional=%.2f USD  response=%s",
                   approved ? "APPROVED" : "REJECTED",
                   dir_str, notional, buf);

        if (approved) {
            std::fprintf(stderr, "  ✓ Order approved — %.2f USD in play\n\n", notional);
        } else {
            std::fprintf(stderr, "  ✗ Order rejected by operator\n\n");
        }

        return approved;
    }

    void print_block(const char* dir_str, double notional,
                     const StrategyOrderSignal& sig) noexcept {
        std::fprintf(stderr,
            "\n"
            "  ╔══════════════════════════════════════════════════╗\n"
            "  ║              ██  HARD BLOCK  ██                  ║\n"
            "  ╠══════════════════════════════════════════════════╣\n"
            "  ║  Direction   : %-34s║\n"
            "  ║  Notional    : %-12.2f USD                     ║\n"
            "  ║  Hard limit  : %-12.2f USD                     ║\n"
            "  ║  Status      : ORDER BLOCKED — NOT SENT          ║\n"
            "  ╚══════════════════════════════════════════════════╝\n\n",
            dir_str, notional, cfg_.hard_block_usd
        );
        (void)sig;
    }

    NotionalGateConfig cfg_;
    double             lot_size_;
};

} // namespace oms
