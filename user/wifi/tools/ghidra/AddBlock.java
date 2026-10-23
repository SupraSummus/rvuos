// Adds a file as an initialized, executable memory block: arguments NAME FILE BASE, such as rom wlan-rom.bin 0x800000.
// Run as a pre-script on an import, so that analysis follows calls into the block.
//@category rvuos
import ghidra.app.script.GhidraScript;
import ghidra.program.database.mem.FileBytes;
import ghidra.program.model.mem.MemoryBlock;
import java.io.File;
import java.io.FileInputStream;

public class AddBlock extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        File f = new File(a[1]);
        FileBytes fb;
        try (FileInputStream in = new FileInputStream(f)) {
            fb = currentProgram.getMemory().createFileBytes(f.getName(), 0, f.length(), in, monitor);
        }
        MemoryBlock b = currentProgram.getMemory().createInitializedBlock(a[0], toAddr(Long.decode(a[2])), fb, 0,
                f.length(), false);
        b.setRead(true);
        b.setExecute(true);
        b.setWrite(false);
        println("block " + a[0] + " at " + a[2] + ", " + f.length() + " bytes");
    }
}
