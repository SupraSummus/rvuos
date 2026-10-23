// Writes the decompiled C of the function holding each address to the file named by the first argument.
//@category rvuos
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;
import java.util.regex.*;

public class Decompile extends GhidraScript {
    // A literal-pool word read as DAT_xxxxxxxx becomes its value; _DAT_ (a global the code reads or writes) stays.
    private static final Pattern POOL = Pattern.compile("(?<![_A-Za-z0-9])DAT_([0-9a-f]{8})");

    private String fold(String c) throws Exception {
        Matcher m = POOL.matcher(c);
        StringBuilder sb = new StringBuilder();
        while (m.find()) {
            Address a = toAddr(Long.parseLong(m.group(1), 16));
            String v = m.group();
            if (currentProgram.getMemory().contains(a) && currentProgram.getMemory().contains(a.add(3)))
                v = String.format("0x%08x", getInt(a) & 0xffffffffL);
            m.appendReplacement(sb, Matcher.quoteReplacement(v));
        }
        m.appendTail(sb);
        return sb.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Address a;
                if (args[i].matches("(0x)?[0-9a-fA-F]+")) a = toAddr(Long.decode(args[i].startsWith("0x") ? args[i] : "0x" + args[i]));
                else {
                    var s = currentProgram.getSymbolTable().getGlobalSymbols(args[i]);
                    if (s.isEmpty()) { out.println("// no symbol " + args[i]); continue; }
                    a = s.get(0).getAddress();
                }
                Function f = getFunctionContaining(a);
                if (f == null) {
                    if (getInstructionAt(a) == null) disassemble(a);
                    f = createFunction(a, null);
                }
                if (f == null) { out.println("// no function at " + args[i]); continue; }
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                out.println("// " + args[i] + ": " + f.getName() + " at " + f.getEntryPoint() + ", called from "
                        + f.getCallingFunctions(monitor).stream().map(c -> c.getEntryPoint().toString()).sorted().toList());
                out.println(r.decompileCompleted() ? fold(r.getDecompiledFunction().getC()) : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
