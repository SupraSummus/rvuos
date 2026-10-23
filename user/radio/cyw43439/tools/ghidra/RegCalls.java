// Lists every call to the helpers named in the arguments, with the arguments as the decompiler gives them:
// first argument the output file, then ADDR=NAME pairs, such as 0x86bf38=phy_mod.
// Each calling function is decompiled once; a line is: caller's entry, caller's name, helper, its arguments.
//@category rvuos
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.*;

public class RegCalls extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Map<String, String> helpers = new LinkedHashMap<>(); // decompiler's name of the helper -> our name
        Set<Function> callers = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
        for (int i = 1; i < args.length; i++) {
            String[] kv = args[i].split("=");
            Address a = toAddr(Long.decode(kv[0]));
            Function h = getFunctionAt(a);
            if (h == null) h = createFunction(a, null);
            if (h == null) { println("no function at " + kv[0]); continue; }
            helpers.put(h.getName(), kv[1]);
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) callers.add(f);
            }
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        int calls = 0;
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (Function f : callers) {
                DecompileResults res = d.decompileFunction(f, 60, monitor);
                if (!res.decompileCompleted()) { out.printf("%s %s ?decompile failed%n", f.getEntryPoint(), f.getName()); continue; }
                String c = res.getDecompiledFunction().getC();
                for (var h : helpers.entrySet()) {
                    String key = h.getKey() + "(";
                    for (int at = c.indexOf(key); at >= 0; at = c.indexOf(key, at + 1)) {
                        int depth = 0, end = at + key.length() - 1;
                        for (; end < c.length(); end++) {
                            char ch = c.charAt(end);
                            if (ch == '(') depth++;
                            else if (ch == ')' && --depth == 0) break;
                        }
                        String a = c.substring(at + key.length(), Math.min(end, c.length())).replaceAll("\\s+", " ");
                        out.printf("%s %s %s %s%n", f.getEntryPoint(), f.getName(), h.getValue(), a);
                        calls++;
                    }
                }
            }
            out.println("// " + calls + " calls in " + callers.size() + " functions");
        }
        println(calls + " calls");
    }
}
