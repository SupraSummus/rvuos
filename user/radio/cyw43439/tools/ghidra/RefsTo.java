// Writes every reference to each address (reads, writes, calls, jumps) to the file named by the first argument,
// with the function each comes from; for a register window or a RAM field, give its exact address.
//@category rvuos
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                Address a;
                if (args[i].matches("(0x)?[0-9a-fA-F]+")) a = toAddr(Long.decode(args[i].startsWith("0x") ? args[i] : "0x" + args[i]));
                else {
                    var s = currentProgram.getSymbolTable().getGlobalSymbols(args[i]);
                    if (s.isEmpty()) { out.println("// no symbol " + args[i]); continue; }
                    a = s.get(0).getAddress();
                }
                out.println("// references to " + args[i]);
                int n = 0;
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.printf("%s  %-14s in %s%n", r.getFromAddress(), r.getReferenceType(),
                            f == null ? "?" : f.getName() + " at " + f.getEntryPoint());
                    n++;
                }
                out.println("// " + n + " references");
            }
        }
    }
}
