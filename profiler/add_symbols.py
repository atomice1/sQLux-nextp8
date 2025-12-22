#!/usr/bin/env python3
"""
Add function and line information to callgrind profile.
Re-read profiler.md for implementation details.

This script reads a callgrind file with only instruction addresses,
uses m68k-elf-addr2line to translate addresses to function names and line numbers,
and writes an enhanced callgrind file with function and line information.
"""

import argparse
import subprocess
import re
import sys
from pathlib import Path
from typing import Dict, Tuple, Optional

class SymbolResolver:
    """Resolves addresses to function names and line numbers using addr2line."""

    def __init__(self, executable_path: str, rom_path: Optional[str] = None):
        self.executable_path = executable_path
        self.rom_path = rom_path
        self.cache: Dict[int, Tuple[str, str, int, str]] = {}  # (func, file, line, source)
        # Keep addr2line processes open for reuse
        self.app_process = None
        self.rom_process = None
        self._start_processes()

    def _start_processes(self):
        """Start addr2line processes for app and ROM."""
        try:
            self.app_process = subprocess.Popen(
                ['m68k-elf-addr2line', '-e', self.executable_path, '-f', '-C'],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1
            )
        except FileNotFoundError:
            print("Warning: m68k-elf-addr2line not found", file=sys.stderr)
            self.app_process = None

        if self.rom_path:
            try:
                self.rom_process = subprocess.Popen(
                    ['m68k-elf-addr2line', '-e', self.rom_path, '-f', '-C'],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1
                )
            except FileNotFoundError:
                print("Warning: m68k-elf-addr2line not found for ROM", file=sys.stderr)
                self.rom_process = None

    def __del__(self):
        """Clean up addr2line processes."""
        if self.app_process:
            self.app_process.terminate()
            self.app_process.wait()
        if self.rom_process:
            self.rom_process.terminate()
            self.rom_process.wait()

    def resolve_address(self, address: int) -> Tuple[str, str, int, str]:
        """
        Resolve an address to (function_name, file_name, line_number, source).
        source is 'app', 'rom', or 'unknown'.
        Returns ('??', '??', 0, 'unknown') if resolution fails.
        """
        # Special handling for synthetic root
        if address == 0xFFFFFFFF:
            return ('<program root>', '<synthetic>', 0, 'synthetic')

        if address in self.cache:
            return self.cache[address]

        # Try the main executable first
        func, file, line = self._try_addr2line(self.app_process, address)
        if func != '??':
            result = (func, file, line, 'app')
        elif self.rom_path and self.rom_process:
            # Try the ROM
            func, file, line = self._try_addr2line(self.rom_process, address)
            if func != '??':
                result = (func, file, line, 'rom')
            else:
                result = (func, file, line, 'unknown')
        else:
            result = (func, file, line, 'unknown')

        self.cache[address] = result
        return result

    def _try_addr2line(self, process: subprocess.Popen, address: int) -> Tuple[str, str, int]:
        """Try to resolve address using a running addr2line process."""
        if not process:
            return ('??', '??', 0)

        try:
            # Send address to addr2line
            process.stdin.write(f'0x{address:x}\n')
            process.stdin.flush()

            # Read function name
            function_name = process.stdout.readline().strip()
            # Read location
            location = process.stdout.readline().strip()

            if not function_name or not location:
                return ('??', '??', 0)

            # Parse location (format: "file:line" or "??:?")
            if ':' in location:
                parts = location.rsplit(':', 1)
                file_name = parts[0]
                try:
                    line_number = int(parts[1])
                except (ValueError, IndexError):
                    line_number = 0
            else:
                file_name = '??'
                line_number = 0

            return (function_name, file_name, line_number)

        except Exception as e:
            print(f"Warning: addr2line failed for address 0x{address:x}: {e}", file=sys.stderr)
            return ('??', '??', 0)


class CallgrindEnhancer:
    """Enhances a callgrind file with function and line information."""

    def __init__(self, resolver: SymbolResolver, executable_path: str, rom_path: Optional[str] = None):
        self.resolver = resolver
        self.executable_path = executable_path
        self.rom_path = rom_path
        self.function_map: Dict[str, int] = {}  # qualified_function_name -> ID
        self.file_map: Dict[str, int] = {}  # file_name -> ID
        self.object_map: Dict[str, int] = {}  # object_name -> ID
        self.address_to_function: Dict[int, str] = {}  # address -> qualified_function_name (1:1 mapping)
        self.function_to_addresses: Dict[str, list] = {}  # base_function_name -> [addresses]
        self.next_fn_id = 1
        self.next_fl_id = 1
        self.next_ob_id = 1

    def _qualify_function_name(self, address: int, function_name: str) -> str:
        """Add discriminator if multiple addresses map to the same function name."""
        if function_name == '??':
            return f'{function_name} (0x{address:x})'

        # Check if we already have a qualified name for this address
        if address in self.address_to_function:
            return self.address_to_function[address]

        # Check if this function name is used by other addresses
        if function_name in self.function_to_addresses:
            # Add address discriminator to make it unique
            qualified_name = f"{function_name} (0x{address:x})"
        else:
            # First address for this function name
            qualified_name = function_name

        # Record the mapping
        self.address_to_function[address] = qualified_name
        if function_name not in self.function_to_addresses:
            self.function_to_addresses[function_name] = []
        self.function_to_addresses[function_name].append(address)

        # If this is the second address for this function, we need to retroactively
        # add discriminator to the first one
        if len(self.function_to_addresses[function_name]) == 2:
            first_addr = self.function_to_addresses[function_name][0]
            old_qualified = self.address_to_function[first_addr]
            if old_qualified == function_name:  # Only if it doesn't already have discriminator
                new_qualified = f"{function_name} (0x{first_addr:x})"
                # Update the mapping
                old_id = self.function_map.pop(old_qualified, None)
                self.address_to_function[first_addr] = new_qualified
                if old_id is not None:
                    self.function_map[new_qualified] = old_id

        return qualified_name

    def get_or_create_function_id(self, function_name: str) -> int:
        """Get or create an ID for a function name."""
        if function_name not in self.function_map:
            self.function_map[function_name] = self.next_fn_id
            self.next_fn_id += 1
        return self.function_map[function_name]

    def get_or_create_file_id(self, file_name: str) -> int:
        """Get or create an ID for a file name."""
        if file_name not in self.file_map:
            self.file_map[file_name] = self.next_fl_id
            self.next_fl_id += 1
        return self.file_map[file_name]

    def get_or_create_object_id(self, source: str) -> int:
        """Get or create an ID for an object (ROM or app)."""
        if source == 'rom':
            obj_name = str(Path(self.rom_path).resolve()) if self.rom_path else 'ROM'
        elif source == 'app':
            obj_name = str(Path(self.executable_path).resolve())
        else:
            obj_name = '<unknown>'

        if obj_name not in self.object_map:
            self.object_map[obj_name] = self.next_ob_id
            self.next_ob_id += 1
        return self.object_map[obj_name]

    def enhance_file(self, input_path: str, output_path: str):
        """Read callgrind file, enhance it, and write to output."""
        with open(input_path, 'r') as f:
            lines = f.readlines()

        # First pass: collect all addresses to pre-populate mappings
        self._collect_all_addresses(lines)

        output_lines = []
        current_function = None
        current_file = None
        current_object = None

        # Collect all header information first (scan until we hit a non-header line)
        header_lines = []
        events_line = None
        summary_line = None
        i = 0

        while i < len(lines):
            line = lines[i].rstrip('\n')

            # Check if this is a header-type line
            # Header lines are: empty, comments, cmd, pid, thread, part, desc, event, events, positions, summary, totals, version, creator
            if line == '' or line.startswith('#'):
                header_lines.append(line)
            elif line.startswith('cmd:') or line.startswith('pid:') or \
                 line.startswith('thread:') or line.startswith('part:') or \
                 line.startswith('desc:') or line.startswith('event:') or \
                 line.startswith('version:') or line.startswith('creator:') or \
                 line.startswith('totals:'):
                header_lines.append(line)
            elif line.startswith('events:'):
                events_line = line
            elif line.startswith('summary:'):
                summary_line = line
            elif line.startswith('positions:'):
                # Skip positions line - we'll write our own
                pass
            else:
                # Hit a non-header line (e.g., fn=, ob=, fl=, cost line), stop collecting
                break

            i += 1

        # Now write header in correct callgrind order
        output_lines.extend(header_lines)
        # Write positions with both instr and line
        output_lines.append('positions: instr line')
        if events_line:
            output_lines.append(events_line)
        if summary_line:
            output_lines.append(summary_line)
        output_lines.append('')  # Blank line after header

        # Write mappings immediately after header
        output_lines.extend(self._write_mappings())
        output_lines.append('')  # Blank line after mappings

        # Process remaining lines (function data, cost lines, etc.)
        first_function = True
        last_address = None  # Track last absolute address for relative addressing
        while i < len(lines):
            line = lines[i].rstrip('\n')

            # Handle function specifications
            if line.startswith('fn='):
                # Extract address from fn=0xADDRESS
                match = re.match(r'fn=0x([0-9a-fA-F]+)', line)
                if match:
                    addr = int(match.group(1), 16)
                    func, file, _, source = self.resolver.resolve_address(addr)
                    qualified_func = self._qualify_function_name(addr, func)
                    fn_id = self.get_or_create_function_id(qualified_func)
                    fl_id = self.get_or_create_file_id(file)
                    ob_id = self.get_or_create_object_id(source)

                    # Always write ob= and fl= for the first function, or when they change
                    if first_function or source != current_object:
                        output_lines.append(f'ob=({ob_id})')
                        current_object = source

                    if first_function or file != current_file:
                        output_lines.append(f'fl=({fl_id})')
                        current_file = file

                    # Update current function (fn=)
                    if first_function or qualified_func != current_function:
                        output_lines.append(f'fn=({fn_id})')
                        current_function = qualified_func

                    first_function = False
                else:
                    # Keep non-address fn= lines (shouldn't happen)
                    output_lines.append(line)

                i += 1
                continue

            # Handle cost lines
            if re.match(r'^[0-9+\-*x]', line):
                address, rest = self._parse_cost_line_with_relative(line, last_address)
                if address is not None:
                    # Write enhanced cost line with line number
                    func, file, lineno, source = self.resolver.resolve_address(address)
                    output_lines.append(f'0x{address:x} {lineno} {rest}')
                    last_address = address
                else:
                    # Keep line as-is if we can't parse it
                    output_lines.append(line)

                i += 1
                continue

            # Handle call specifications
            if line.startswith('calls='):
                # Extract target address from calls= line
                match = re.match(r'calls=(\d+)\s+(0x[0-9a-fA-F]+|\d+)', line)
                if match:
                    count = match.group(1)
                    target_addr_str = match.group(2)
                    if target_addr_str.startswith('0x'):
                        target_addr = int(target_addr_str, 16)
                    else:
                        target_addr = int(target_addr_str)

                    # Resolve target function
                    target_func, target_file, target_line, target_source = self.resolver.resolve_address(target_addr)
                    qualified_target = self._qualify_function_name(target_addr, target_func)
                    target_fn_id = self.get_or_create_function_id(qualified_target)
                    target_fl_id = self.get_or_create_file_id(target_file)
                    target_ob_id = self.get_or_create_object_id(target_source)

                    # Write cob= (if object changed)
                    if target_source != current_object:
                        output_lines.append(f'cob=({target_ob_id})')

                    # Write cfi= (if file changed)
                    if target_file != current_file:
                        output_lines.append(f'cfi=({target_fl_id})')

                    # Write cfn=
                    output_lines.append(f'cfn=({target_fn_id})')
                    output_lines.append(f'calls={count} 0x{target_addr:x} {target_line}')
                else:
                    output_lines.append(line)

                i += 1
                continue

            # Handle cfn= lines (skip them, we'll generate new ones)
            if line.startswith('cfn=') or line.startswith('cfl=') or line.startswith('cfi=') or \
               line.startswith('cob=') or line.startswith('ob='):
                i += 1
                continue

            # Keep everything else as-is
            output_lines.append(line)
            i += 1

        # Write output file
        with open(output_path, 'w') as f:
            for line in output_lines:
                f.write(line + '\n')

        print(f"Enhanced callgrind file written to {output_path}")
        print(f"Resolved {len(self.function_map)} functions and {len(self.file_map)} files")

    def _collect_all_addresses(self, lines: list):
        """Pre-scan file to collect all function entry points and create ID mappings."""
        # Collect only function entry points (addresses from fn= lines)
        function_entries = set()

        for line in lines:
            line = line.rstrip('\n')

            # Parse fn= lines for function entry addresses
            if line.startswith('fn=0x'):
                match = re.match(r'fn=0x([0-9a-fA-F]+)', line)
                if match:
                    function_entries.add(int(match.group(1), 16))

            # Parse calls= and cfn= lines for call target addresses
            # These are also function entry points
            if line.startswith('calls='):
                match = re.match(r'calls=(\d+)\s+(0x[0-9a-fA-F]+|\d+)', line)
                if match:
                    target_addr_str = match.group(2)
                    if target_addr_str.startswith('0x'):
                        target_addr = int(target_addr_str, 16)
                    else:
                        target_addr = int(target_addr_str)
                    function_entries.add(target_addr)

            if line.startswith('cfn=0x'):
                match = re.match(r'cfn=0x([0-9a-fA-F]+)', line)
                if match:
                    function_entries.add(int(match.group(1), 16))

        # Create qualified names and ID mappings ONLY for function entry points
        for addr in function_entries:
            func, file, _, source = self.resolver.resolve_address(addr)
            qualified_func = self._qualify_function_name(addr, func)
            self.get_or_create_function_id(qualified_func)
            self.get_or_create_file_id(file)
            self.get_or_create_object_id(source)

    def _write_mappings(self) -> list:
        """Write function and file ID mappings."""
        lines = []
        lines.append('')

        lines.append('# Object mappings')
        for obj, ob_id in sorted(self.object_map.items(), key=lambda x: x[1]):
            lines.append(f'ob=({ob_id}) {obj}')

        lines.append('')
        lines.append('# Function mappings')
        for func, fn_id in sorted(self.function_map.items(), key=lambda x: x[1]):
            lines.append(f'fn=({fn_id}) {func}')

        lines.append('')
        lines.append('# File mappings')
        for file, fl_id in sorted(self.file_map.items(), key=lambda x: x[1]):
            lines.append(f'fl=({fl_id}) {file}')

        lines.append('')
        return lines

    def _parse_cost_line(self, line: str) -> Tuple[Optional[int], str]:
        """
        Parse a cost line to extract the instruction address and the cost values.
        Input format (positions: instr): 0xADDR cost1 cost2 cost3 cost4
        Returns (address, rest) where rest contains only the cost values.
        """
        parts = line.split(None, 1)
        if not parts:
            return (None, line)

        addr_str = parts[0]
        rest = parts[1] if len(parts) > 1 else ''

        # Parse address (can be absolute or relative)
        if addr_str.startswith('0x'):
            try:
                address = int(addr_str, 16)
                return (address, rest)
            except ValueError:
                return (None, line)
        elif addr_str == '*':
            # Same as previous - we can't handle this without tracking state
            return (None, line)
        elif addr_str.startswith('+') or addr_str.startswith('-'):
            # Relative address - we can't handle this without tracking state
            return (None, line)
        else:
            # Try parsing as decimal
            try:
                address = int(addr_str)
                return (address, rest)
            except ValueError:
                return (None, line)

    def _parse_cost_line_with_relative(self, line: str, last_address: Optional[int]) -> Tuple[Optional[int], str]:
        """
        Parse a cost line supporting relative addressing.
        Input format (positions: instr): <addr> cost1 cost2 cost3 cost4
        Returns (absolute_address, rest) where rest contains only the cost values.
        """
        parts = line.split(None, 1)
        if not parts:
            return (None, line)

        addr_str = parts[0]
        rest = parts[1] if len(parts) > 1 else ''

        # Parse address (can be absolute or relative)
        if addr_str.startswith('0x'):
            try:
                address = int(addr_str, 16)
                return (address, rest)
            except ValueError:
                return (None, line)
        elif addr_str == '*':
            # Same as previous
            return (last_address, rest) if last_address is not None else (None, line)
        elif addr_str.startswith('+'):
            # Positive relative
            try:
                offset = int(addr_str[1:])
                if last_address is not None:
                    return (last_address + offset, rest)
                else:
                    return (None, line)
            except ValueError:
                return (None, line)
        elif addr_str.startswith('-'):
            # Negative relative
            try:
                offset = int(addr_str)  # Already includes the minus sign
                if last_address is not None:
                    return (last_address + offset, rest)
                else:
                    return (None, line)
            except ValueError:
                return (None, line)
        else:
            # Try parsing as decimal
            try:
                address = int(addr_str)
                return (address, rest)
            except ValueError:
                return (None, line)


def main():
    parser = argparse.ArgumentParser(
        description='Add function and line information to callgrind profile'
    )
    parser.add_argument(
        'callgrind_file',
        help='Path to the callgrind file to enhance'
    )
    parser.add_argument(
        'executable',
        help='Path to the application executable (ELF file)'
    )
    parser.add_argument(
        '--rom',
        help='Optional path to ROM ELF file',
        default=None
    )
    parser.add_argument(
        '--output',
        help='Output file (default: overwrite input file)',
        default=None
    )

    args = parser.parse_args()

    # Validate input files
    if not Path(args.callgrind_file).exists():
        print(f"Error: Callgrind file not found: {args.callgrind_file}", file=sys.stderr)
        return 1

    if not Path(args.executable).exists():
        print(f"Error: Executable not found: {args.executable}", file=sys.stderr)
        return 1

    if args.rom and not Path(args.rom).exists():
        print(f"Error: ROM file not found: {args.rom}", file=sys.stderr)
        return 1

    # Determine output file
    output_file = args.output if args.output else args.callgrind_file

    # Create resolver and enhancer
    resolver = SymbolResolver(args.executable, args.rom)
    enhancer = CallgrindEnhancer(resolver, args.executable, args.rom)

    # Process the file
    try:
        enhancer.enhance_file(args.callgrind_file, output_file)
        return 0
    except Exception as e:
        print(f"Error processing file: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1


if __name__ == '__main__':
    sys.exit(main())
