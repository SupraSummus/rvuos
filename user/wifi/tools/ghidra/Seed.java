// Disassembles and creates a function at every address in the file given as the script's argument, then reanalyzes.
//@category rvuos
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.nio.file.*;

public class Seed extends GhidraScript {
    @Override
    public void run() throws Exception {
        int made = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Address a = toAddr(Long.decode(line));
            if (getFunctionAt(a) != null) continue;
            if (getInstructionAt(a) == null) disassemble(a);
            if (createFunction(a, null) != null) made++;
        }
        analyzeAll(currentProgram);
        println("seeded " + made + " functions");
    }
}
