// Names each unnamed function after the one identifier string that only it refers to, its __func__ as a rule:
// a string of [a-z0-9_] with an underscore, reached through a pointer the function loads from its literal pool.
// Writes every name given, and every function left with several candidates, to the file named by the first argument.
//@category rvuos
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class NameFromStrings extends GhidraScript {
    private String identifierAt(Memory mem, Address a) {
        StringBuilder s = new StringBuilder();
        try {
            for (int i = 0; i < 80; i++) {
                int c = mem.getByte(a.add(i)) & 0xff;
                if (c == 0) break;
                if (!(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_')) return null;
                s.append((char) c);
            }
        } catch (Exception e) {
            return null;
        }
        String t = s.toString();
        return t.length() >= 5 && t.contains("_") && Character.isLetter(t.charAt(0)) ? t : null;
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Map<Function, Set<String>> byFunction = new HashMap<>();
        Map<String, Set<Function>> users = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    Address pool = r.getToAddress();
                    if (!mem.contains(pool)) continue;
                    String id;
                    try {
                        long p = mem.getInt(pool) & 0xffffffffL;
                        Address target = toAddr(p);
                        id = mem.contains(target) ? identifierAt(mem, target) : null;
                    } catch (Exception e) {
                        id = null;
                    }
                    if (id == null) continue;
                    byFunction.computeIfAbsent(f, k -> new TreeSet<>()).add(id);
                    users.computeIfAbsent(id, k -> new HashSet<>()).add(f);
                }
            }
        }
        int named = 0;
        try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
            for (var e : byFunction.entrySet()) {
                Function f = e.getKey();
                List<String> own = new ArrayList<>();
                for (String id : e.getValue()) if (users.get(id).size() == 1) own.add(id);
                if (own.size() == 1 && f.getName().startsWith("FUN_")) {
                    f.setName(own.get(0), SourceType.ANALYSIS);
                    out.printf("%s %s%n", f.getEntryPoint(), own.get(0));
                    named++;
                } else if (own.size() > 1) {
                    out.printf("%s %s ambiguous %s%n", f.getEntryPoint(), f.getName(), own);
                }
            }
            out.println("// named " + named);
        }
        println("named " + named);
    }
}
