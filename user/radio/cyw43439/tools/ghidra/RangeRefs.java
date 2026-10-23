// For each range LO-HI, writes every function that reads or writes into it, with its counts and the addresses
// it touches, busiest first, to the file named by the first argument.
//@category rvuos
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class RangeRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        ReferenceManager rm = currentProgram.getReferenceManager();
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; i++) {
                String[] lohi = args[i].split("-");
                AddressSet set = new AddressSet(toAddr(Long.decode(lohi[0])), toAddr(Long.decode(lohi[1])));
                Map<String, int[]> counts = new HashMap<>();
                Map<String, TreeSet<String>> touched = new HashMap<>();
                AddressIterator it = rm.getReferenceDestinationIterator(set, true);
                while (it.hasNext()) {
                    Address to = it.next();
                    for (Reference r : rm.getReferencesTo(to)) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        String key = f == null ? "?" : f.getEntryPoint().toString();
                        int[] c = counts.computeIfAbsent(key, k -> new int[2]);
                        if (r.getReferenceType().isWrite()) c[0]++; else c[1]++;
                        touched.computeIfAbsent(key, k -> new TreeSet<>()).add(to.toString());
                    }
                }
                out.println("// " + args[i] + ": " + counts.size() + " functions; writes, reads, addresses touched");
                counts.entrySet().stream()
                        .sorted((a, b) -> (b.getValue()[0] + b.getValue()[1]) - (a.getValue()[0] + a.getValue()[1]))
                        .forEach(e -> out.printf("%s  w%-3d r%-3d %s%n", e.getKey(), e.getValue()[0], e.getValue()[1],
                                String.join(" ", touched.get(e.getKey()))));
            }
        }
    }
}
