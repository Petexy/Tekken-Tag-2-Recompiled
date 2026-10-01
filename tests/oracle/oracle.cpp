#include "oracle.h"

#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// Definitions that live in Dolphin files the oracle does not build.
Interpreter::Interpreter(Core::System& system, PowerPC::PowerPCState& ppc_state,
                         PowerPC::MMU& mmu, Core::BranchWatch& branch_watch,
                         PPCSymbolDB& ppc_symbol_db)
    : m_system(system), m_ppc_state(ppc_state), m_mmu(mmu), m_branch_watch(branch_watch),
      m_ppc_symbol_db(ppc_symbol_db) {}
Interpreter::~Interpreter() = default;

double PowerPC::PairedSingle::PS0AsDouble() const { return std::bit_cast<double>(ps0); }
double PowerPC::PairedSingle::PS1AsDouble() const { return std::bit_cast<double>(ps1); }
void PowerPC::PairedSingle::SetPS0(double value) { ps0 = std::bit_cast<u64>(value); }
void PowerPC::PairedSingle::SetPS1(double value) { ps1 = std::bit_cast<u64>(value); }
void PowerPC::PowerPCState::UpdateFPRFDouble(double value) { fpscr.FPRF = Common::ClassifyDouble(value); }
void PowerPC::PowerPCState::UpdateFPRFSingle(float value) { fpscr.FPRF = Common::ClassifyFloat(value); }

void Interpreter::unknown_instruction(Interpreter& interpreter, UGeckoInstruction) {
    interpreter.m_ppc_state.Exceptions |= EXCEPTION_PROGRAM;
}

// Branches and system calls are tested outside the oracle; Interpreter_Branch
// drags in Dolphin's HLE layer, so it is not built.
#define ORACLE_UNTESTED(name)                                                  \
    void Interpreter::name(Interpreter& interpreter, UGeckoInstruction) {      \
        interpreter.m_ppc_state.Exceptions |= EXCEPTION_PROGRAM;               \
    }
ORACLE_UNTESTED(bx)
ORACLE_UNTESTED(bcx)
ORACLE_UNTESTED(bcctrx)
ORACLE_UNTESTED(bclrx)
ORACLE_UNTESTED(HLEFunction)
ORACLE_UNTESTED(rfi)
ORACLE_UNTESTED(sc)

bool oracle_execute(OracleState& s, uint8_t* memory, uint32_t word) {
    PowerPC::MMU mmu(memory);
    Core::System system(mmu);
    Core::BranchWatch branch_watch;
    PPCSymbolDB symbols;
    PowerPC::PowerPCState ppc{};
    Interpreter interpreter(system, ppc, mmu, branch_watch, symbols);

    for (int i = 0; i < 32; ++i) {
        ppc.gpr[i] = s.gpr[i];
        ppc.ps[i].ps0 = s.ps0[i];
        ppc.ps[i].ps1 = s.ps1[i];
    }
    for (u32 i = 0; i < 8; ++i) {
        ppc.cr.SetField(i, s.cr[i]);
        ppc.spr[SPR_GQR0 + i] = s.gqr[i];
        ppc.spr[896 + i] = s.gqr[i]; // Espresso's user-mode GQR aliases
    }
    ppc.xer_so_ov = static_cast<u8>((s.so << 1) | s.ov);
    ppc.xer_ca = s.ca;
    ppc.xer_stringctrl = s.bc;
    ppc.spr[SPR_LR] = s.lr;
    ppc.spr[SPR_CTR] = s.ctr;
    // Paired singles and quantized loads enabled, as on the Espresso.
    UReg_HID2 hid2{0};
    hid2.PSE = 1;
    hid2.LSQE = 1;
    ppc.spr[SPR_HID2] = hid2.Hex;
    ppc.msr.FP = 1;

    const UGeckoInstruction inst{word};
    Interpreter::GetInterpreterOp(inst)(interpreter, inst);
    if (ppc.Exceptions != 0) {
        return false;
    }

    for (int i = 0; i < 32; ++i) {
        s.gpr[i] = ppc.gpr[i];
        s.ps0[i] = ppc.ps[i].ps0;
        s.ps1[i] = ppc.ps[i].ps1;
    }
    for (u32 i = 0; i < 8; ++i) {
        s.cr[i] = static_cast<uint8_t>(ppc.cr.GetField(i));
        // A write through either alias is the new value.
        s.gqr[i] = ppc.spr[896 + i] != s.gqr[i] ? ppc.spr[896 + i] : ppc.spr[SPR_GQR0 + i];
    }
    s.so = static_cast<uint8_t>(ppc.GetXER_SO());
    s.ov = static_cast<uint8_t>(ppc.GetXER_OV());
    s.ca = ppc.xer_ca;
    s.bc = static_cast<uint8_t>(ppc.xer_stringctrl & 0x7F);
    s.lr = ppc.spr[SPR_LR];
    s.ctr = ppc.spr[SPR_CTR];
    return true;
}
