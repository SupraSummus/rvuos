// Marks every memory block read-only, so the decompiler folds literal-pool constants into their values.
//@category rvuos
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class ReadOnly extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) b.setWrite(false);
    }
}
