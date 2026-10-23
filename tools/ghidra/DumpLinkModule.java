// Dump private TC3 link firmware pseudocode into a caller-owned evidence directory.
//@category TC3
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import java.nio.file.Path;
import java.nio.file.Files;
import java.io.PrintWriter;

public class DumpLinkModule extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    if(args.length != 1) throw new IllegalArgumentException("Output directory required");
    Path output=Path.of(args[0]); Files.createDirectories(output);
    // Export-table entries may not have a direct caller inside the module.
    // Original IRX export signature is 0x41c00000, followed by header/name/table.
    for (var block:currentProgram.getMemory().getBlocks()) {
      if(!block.isInitialized()) continue;
      long begin=block.getStart().getOffset(), end=block.getEnd().getOffset();
      for(long off=begin;off+24<=end;off+=4) {
        if(getInt(toAddr(off)) != 0x41c00000) continue;
        long entry=off+20;
        for(int index=0;entry+4<=end && index<256;entry+=4,index++) {
          long target=Integer.toUnsignedLong(getInt(toAddr(entry)));
          if(target==0 && index>3) break;
          Address address=toAddr(target);
          if(getInstructionAt(address)==null) disassemble(address);
          if(getFunctionAt(address)==null) createFunction(address,"export_"+index);
        }
      }
    }
    DecompInterface decompiler=new DecompInterface(); decompiler.openProgram(currentProgram);
    try(PrintWriter out=new PrintWriter(output.resolve(currentProgram.getName()+".c").toFile())) {
      out.println("// Static Ghidra pseudocode, original module: "+currentProgram.getExecutablePath());
      for(Function f:currentProgram.getFunctionManager().getFunctions(true)) {
        if(monitor.isCancelled()) break;
        out.println("\n// FUNCTION "+f.getEntryPoint()+" "+f.getName());
        var r=decompiler.decompileFunction(f,20,monitor);
        if(r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
        else out.println("// DECOMPILE ERROR "+r.getErrorMessage());
      }
    } finally {decompiler.dispose();}
  }
}
