#pragma once

#include "peripheral_base.hpp"

namespace ymir::peripheral {

/// @brief Implements the Saturn Control Pad (ID 0x0/2 bytes) with:
/// - 6 digital buttons: ABC XYZ
/// - 2 shoulder buttons: L R
/// - Directional pad
/// - Start button
class ControlPad final : public BasePeripheral {
public:
    explicit ControlPad(CBPeripheralReport callback);

    /// Inject a button state for the next peripheral report.
    void SetButtons(Button buttons);

    /// Override every peripheral report until ClearPersistentButtons().
    void SetPersistentButtons(Button buttons);
    void ClearPersistentButtons();

    void UpdateInputs() override;

    [[nodiscard]] uint8 GetReportLength() const override;

    void Read(std::span<uint8> out) override;

    [[nodiscard]] uint8 WritePDR(uint8 ddr, uint8 value, bool exle) override;

private:
    ControlPadReport m_report;
    Button m_forcedButtons{Button::Default};
    bool m_forceButtons{false};
    Button m_persistentButtons{Button::Default};
    bool m_persistButtons{false};
};

} // namespace ymir::peripheral
