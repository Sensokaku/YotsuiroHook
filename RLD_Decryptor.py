#!/usr/bin/env python3
"""
RLD Tool for Yotsuiro Passionato
Decrypt → Parse → Extract → Translation-Ready Output
v3: Character ID lookup from defChara.rld
"""
import struct
import os
import sys
import json
import re
from pathlib import Path

#=============================================================================
# Known Seeds
#=============================================================================
KNOWN_SEEDS = {
    'def.rld': 0xAE85A916,
    '_default': 0x20100806,
}

#=============================================================================
# Command Types (from game's command table)
#=============================================================================
CMD_TYPES = {
    0x00: 'INIT',
    0x01: 'DIRECT',
    0x05: 'BLOCK',
    0x0B: 'EXHIBIT',
    0x0C: 'AUTOSAVE',
    0x0D: 'WAIT',
    0x11: 'CHANGESCENARIO',
    0x14: 'JUMP',
    0x15: 'QUESTION',
    0x16: 'CALL',
    0x17: 'RETURN',
    0x18: 'TERMINATE',
    0x1C: 'MESSAGE',
    0x30: 'CREATECHARACTER',
    0x31: 'DEFCHARACTER',
    0x8C: 'EXITSCENARIO',
    0xBF: 'ACTION',
}

#=============================================================================
# Global Character Table (loaded from defChara.rld)
#=============================================================================
CHAR_TABLE = {}

#=============================================================================
# Text Detection
#=============================================================================
def contains_japanese(text):
    """Check if text contains Japanese characters"""
    if not text:
        return False
    for char in text:
        code = ord(char)
        if (0x3040 <= code <= 0x309F or  # Hiragana
            0x30A0 <= code <= 0x30FF or  # Katakana
            0x4E00 <= code <= 0x9FFF or  # CJK
            0xFF00 <= code <= 0xFFEF):   # Fullwidth
            return True
    return False

def is_translatable_text(text):
    """Check if text is worth translating"""
    if not text:
        return False
    if len(text) < 2:
        return False
    if re.match(r'^[\d,\-\.\*\s\;\:\&\|\=\<\>\[\]\(\)RQLSrqls]+$', text):
        return False
    skip_starts = ['-1,', '0,', '1,', '10,', '100,', '101,', '102,', '2000,']
    for pattern in skip_starts:
        if text.startswith(pattern) and not contains_japanese(text):
            return False
    return contains_japanese(text) or re.search(r'[a-zA-Z]{3,}', text)

def extract_label_text(label_string):
    """Extract translatable portion from LABEL string"""
    if not label_string:
        return None
    parts = label_string.split(',')
    for i in range(len(parts) - 1, -1, -1):
        part = parts[i].strip()
        if contains_japanese(part) and part != '*':
            return part
    return None

#=============================================================================
# Key Table Generation
#=============================================================================
def generate_key_table(seed):
    """Generate 256-entry key table from seed (MT19937-based)"""
    mt = [0] * 624
    a = seed
    idx = 0

    for _ in range(104):
        for _ in range(6):
            if idx >= 624:
                break
            val = a & 0xFFFF0000
            a = (69069 * a + 1) & 0xFFFFFFFF
            val |= (a >> 16)
            mt[idx] = val
            a = (69069 * a + 1) & 0xFFFFFFFF
            idx += 1

    for i in range(227):
        y = (mt[i] & 0x80000000) | (mt[i + 1] & 0x7FFFFFFF)
        mt[i] = mt[i + 397] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)

    for i in range(227, 623):
        y = (mt[i] & 0x80000000) | (mt[i + 1] & 0x7FFFFFFF)
        mt[i] = mt[i - 227] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)

    y = (mt[623] & 0x80000000) | (mt[0] & 0x7FFFFFFF)
    mt[623] = mt[396] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)

    keys = []
    for i in range(256):
        y = mt[i]
        y ^= (y >> 11)
        y ^= ((y << 7) & 0x9D2C5680)
        y ^= ((y << 15) & 0xEFC60000)
        y ^= (y >> 18)
        keys.append((y ^ seed) & 0xFFFFFFFF)

    return keys

#=============================================================================
# Decryption
#=============================================================================
def decrypt_rld(data, keys):
    """Decrypt RLD data"""
    result = bytearray(data)
    block_count = min(len(data), 0xFFCF)
    block_count -= block_count % 4

    key_idx = 0
    for i in range(0x10, block_count, 4):
        if i + 4 <= len(result):
            enc = struct.unpack('<I', result[i:i+4])[0]
            dec = enc ^ keys[key_idx & 0xFF]
            struct.pack_into('<I', result, i, dec)
            key_idx += 1

    return bytes(result)

#=============================================================================
# RLD Parsing
#=============================================================================
def read_cstring(data, offset):
    """Read null-terminated Shift-JIS string"""
    end = offset
    while end < len(data) and data[end] != 0:
        end += 1
    try:
        s = data[offset:end].decode('cp932')
    except:
        s = data[offset:end].decode('cp932', errors='replace')
    return s, end + 1

def parse_rld(data):
    """Parse decrypted RLD into commands"""
    if len(data) < 16 or data[1:4] != b'DLR':
        return []

    cmd_offset = struct.unpack('<I', data[8:12])[0]
    cmd_count = struct.unpack('<I', data[12:16])[0]

    if cmd_offset >= len(data):
        return []

    commands = []
    offset = cmd_offset

    for _ in range(cmd_count):
        if offset + 4 > len(data):
            break

        cmd_start = offset
        header = struct.unpack('<I', data[offset:offset+4])[0]
        offset += 4

        cmd_type = header & 0xFFFF
        dword_count = (header >> 16) & 0xFF
        string_count = (header >> 24) & 0x0F

        if cmd_type > 0x1000 or dword_count > 50 or string_count > 15:
            break

        cmd = {
            'offset': cmd_start,
            'type': cmd_type,
            'type_name': CMD_TYPES.get(cmd_type, f'CMD_{cmd_type:02X}'),
            'params': [],
            'strings': []
        }

        for _ in range(dword_count):
            if offset + 4 <= len(data):
                cmd['params'].append(struct.unpack('<I', data[offset:offset+4])[0])
                offset += 4

        for _ in range(string_count):
            if offset < len(data):
                s, offset = read_cstring(data, offset)
                cmd['strings'].append(s)

        commands.append(cmd)

        if len(commands) > 50000:
            break

    return commands

#=============================================================================
# Parse defChara.rld for Character ID → Name mapping
#=============================================================================
def parse_defchara(data):
    """Parse defChara.rld to extract character ID → name mapping"""
    global CHAR_TABLE

    commands = parse_rld(data)

    for cmd in commands:
        # CMD_CREATECHARACTER (0x30) contains character definitions
        if cmd['type'] == 0x30 and cmd['strings']:
            # Format: "CharID,??,??,Name,,,,,,,,,,"
            parts = cmd['strings'][0].split(',')
            if len(parts) >= 4 and parts[0].isdigit():
                char_id = int(parts[0])
                char_name = parts[3].strip()
                if char_name:
                    CHAR_TABLE[char_id] = char_name

    return CHAR_TABLE

#=============================================================================
# Text Extraction with Character ID Support
#=============================================================================
def extract_translatable(commands, filename):
    """Extract translatable text with proper speaker names and flow tracking"""
    entries = []
    current_branch = None

    for i, cmd in enumerate(commands):
        if cmd['type'] == 0x1C:  # MESSAGE
            if not cmd['strings']:
                continue

            entry = {
                'file': filename,
                'index': i,
                'type': 'MESSAGE',
                'speaker': None,
                'text': None,
                'char_id': 0,
                'branch': current_branch,  # Track which branch we're in
            }

            char_id = 0
            if cmd['params']:
                char_id = cmd['params'][0]
            entry['char_id'] = char_id

            inline_name = None
            if len(cmd['strings']) >= 2:
                if cmd['strings'][0] and cmd['strings'][0] != '*':
                    inline_name = cmd['strings'][0]
                entry['text'] = cmd['strings'][-1]
            elif len(cmd['strings']) == 1:
                entry['text'] = cmd['strings'][0]

            if inline_name:
                entry['speaker'] = inline_name
            elif char_id >= 3 and char_id in CHAR_TABLE:
                entry['speaker'] = CHAR_TABLE[char_id]

            if entry['text'] and is_translatable_text(entry['text']):
                entries.append(entry)

        elif cmd['type'] == 0x05:  # BLOCK / LABEL
            if cmd['strings']:
                raw_label = cmd['strings'][-1] if cmd['strings'] else ''

                # Parse BLOCK format: "id,flags,next,name,*"
                parts = raw_label.split(',')
                block_name = parts[3] if len(parts) >= 4 else ''
                next_block = parts[2] if len(parts) >= 3 else ''

                # Check for choice branch (R####＝N)
                branch_match = re.match(r'^R(\d+)[＝=](\d+)$', block_name.strip())

                if branch_match:
                    question_id = branch_match.group(1)
                    choice_num = branch_match.group(2)
                    current_branch = f"CHOICE_{question_id}_{choice_num}"

                    entries.append({
                        'file': filename,
                        'index': i,
                        'type': 'BRANCH_START',
                        'branch_id': current_branch,
                        'raw': raw_label,
                    })

                elif block_name.strip() == '*' and current_branch:
                    # Merge point - ends all branches
                    entries.append({
                        'file': filename,
                        'index': i,
                        'type': 'MERGE',
                        'raw': raw_label,
                    })
                    current_branch = None

                else:
                    # Regular label
                    extracted = extract_label_text(raw_label)
                    if extracted and is_translatable_text(extracted):
                        if current_branch:
                            current_branch = None

                        entries.append({
                            'file': filename,
                            'index': i,
                            'type': 'LABEL',
                            'text': extracted,
                            'raw': raw_label,
                        })

        elif cmd['type'] == 0x14:  # JUMP (within file)
            if cmd['strings']:
                entries.append({
                    'file': filename,
                    'index': i,
                    'type': 'JUMP',
                    'target': cmd['strings'][0],
                    'branch': current_branch,
                })

        elif cmd['type'] == 0x15:  # QUESTION/CHOICE
            for j, opt_raw in enumerate(cmd['strings']):
                if not opt_raw or opt_raw == '*':
                    continue

                parts = opt_raw.split('\t')
                choice_texts = []

                for part in parts:
                    part = part.strip()
                    if not part or part == '*':
                        continue
                    if part.lstrip('-').replace('.', '').isdigit():
                        continue
                    if part.endswith('.gyu') or part.startswith('res\\'):
                        continue
                    if contains_japanese(part) and is_translatable_text(part):
                        choice_texts.append(part)

                for choice_idx, choice_text in enumerate(choice_texts):
                    entries.append({
                        'file': filename,
                        'index': i,
                        'type': f'CHOICE_{j}_{choice_idx + 1}',
                        'text': choice_text,
                })

        elif cmd['type'] == 0x11:  # CHANGESCENARIO (to another file)
            if cmd['strings']:
                entries.append({
                    'file': filename,
                    'index': i,
                    'type': 'GOTO_FILE',
                    'target': cmd['strings'][0],
                    'branch': current_branch,
                })
            current_branch = None

    return entries

#=============================================================================
# Export Functions
#=============================================================================
def export_tsv(all_entries, output_path):
    """Export translatable text to TSV with flow markers"""
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("# Yotsuiro Passionato Translation File\n")
        f.write("# Save as UTF-8!\n")
        f.write("#\n")
        f.write("# CHOICE_X_N = Choice option N\n")
        f.write("# BRANCH markers show choice flow\n")
        f.write("# Leave NAME translation empty to use unique_names.tsv\n")
        f.write("#\n")
        f.write("FILE\tINDEX\tTYPE\tORIGINAL\tTRANSLATION\n")

        current_file = None
        count = 0

        for entry in all_entries:
            # File header
            if entry['file'] != current_file:
                current_file = entry['file']
                f.write(f"#\n# === {current_file} ===\n#\n")

            # Branch/flow markers (as comments)
            if entry['type'] == 'BRANCH_START':
                f.write(f"#\n#┌──── {entry['branch_id']} ────\n#│\n")
                continue

            if entry['type'] == 'JUMP':
                branch_info = f" (in {entry['branch']})" if entry.get('branch') else ""
                f.write(f"#│\n#└──→ JUMP: {entry['target']}{branch_info}\n#\n")
                continue

            if entry['type'] == 'GOTO_FILE':
                branch_info = f" (in {entry['branch']})" if entry.get('branch') else ""
                f.write(f"#│\n#└══→ GOTO: {entry['target']}{branch_info}\n#\n")
                continue

            if entry['type'] == 'MERGE':
                f.write(f"#│\n#└──────── MERGE ────────\n#\n")
                continue

            # Actual translatable content
            if entry['type'] == 'MESSAGE':
                if entry.get('speaker'):
                    speaker_escaped = entry['speaker'].replace('\n', '\\n').replace('\t', '\\t')
                    f.write(f"{entry['file']}\t{entry['index']}\tNAME\t{speaker_escaped}\t\n")
                    count += 1
                if entry.get('text'):
                    text_escaped = entry['text'].replace('\n', '\\n').replace('\t', '\\t')
                    f.write(f"{entry['file']}\t{entry['index']}\tTEXT\t{text_escaped}\t\n")
                    count += 1

            elif entry['type'] == 'LABEL':
                text_escaped = entry['text'].replace('\n', '\\n').replace('\t', '\\t')
                f.write(f"{entry['file']}\t{entry['index']}\tLABEL\t{text_escaped}\t\n")
                count += 1

            elif entry['type'].startswith('CHOICE_'):
                text_escaped = entry['text'].replace('\n', '\\n').replace('\t', '\\t')
                f.write(f"{entry['file']}\t{entry['index']}\t{entry['type']}\t{text_escaped}\t\n")
                count += 1

    return count

def export_json(all_entries, output_path):
    """Export full data to JSON"""
    clean_entries = []
    for entry in all_entries:
        clean = {k: v for k, v in entry.items() if not k.startswith('_')}
        clean_entries.append(clean)

    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(clean_entries, f, ensure_ascii=False, indent=2)

def export_txt(entries, output_path):
    """Export human-readable script with flow markers"""
    with open(output_path, 'w', encoding='utf-8') as f:
        for entry in entries:
            if entry['type'] == 'BRANCH_START':
                f.write(f"\n{'─'*40}\n")
                f.write(f"▼ {entry['branch_id']}\n")
                f.write(f"{'─'*40}\n\n")

            elif entry['type'] == 'JUMP':
                branch_info = f" [{entry['branch']}]" if entry.get('branch') else ""
                f.write(f"\n  └→ JUMP: {entry['target']}{branch_info}\n\n")

            elif entry['type'] == 'GOTO_FILE':
                branch_info = f" [{entry['branch']}]" if entry.get('branch') else ""
                f.write(f"\n  └═→ GOTO: {entry['target']}{branch_info}\n\n")

            elif entry['type'] == 'LABEL':
                f.write(f"\n{'═'*60}\n")
                f.write(f"【{entry['text']}】\n")
                f.write(f"{'═'*60}\n\n")

            elif entry['type'] == 'MESSAGE':
                speaker = entry.get('speaker')
                text = entry.get('text', '')
                if speaker:
                    f.write(f"【{speaker}】\n")
                f.write(f"{text}\n\n")

            elif entry['type'].startswith('CHOICE_'):
                parts = entry['type'].split('_')
                choice_num = parts[-1] if len(parts) >= 3 else '?'
                f.write(f"▶ [{choice_num}] {entry['text']}\n")

def export_unique_names(all_entries, output_path):
    """Export ALL unique speaker names (inline + defChara)"""
    unique_names = set()

    # Collect from script entries (inline names)
    for entry in all_entries:
        if entry.get('_no_tsv'):
            continue
        if entry['type'] == 'MESSAGE' and entry.get('speaker'):
            name = entry['speaker']
            if name:
                unique_names.add(name)

    # Add names from CHAR_TABLE (defChara.rld)
    for char_id, name in CHAR_TABLE.items():
        if name:
            unique_names.add(name)

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("# Character names - fill TRANSLATION column\n")
        f.write("# Includes names from scripts AND defChara.rld\n")
        f.write("#\n")
        f.write("ORIGINAL\tTRANSLATION\n")

        for name in sorted(unique_names):
            f.write(f"{name}\t\n")

    return len(unique_names)

def export_char_table(output_path):
    """Export character table for reference"""
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("# Character ID Table (from defChara.rld)\n")
        f.write("# ID<TAB>Name\n")
        f.write("#\n")
        for char_id, name in sorted(CHAR_TABLE.items()):
            f.write(f"{char_id}\t{name}\n")

#=============================================================================
# File Processing
#=============================================================================
def process_file(filepath, keys, output_dirs, is_defchara=False):
    """Process single RLD file"""
    filename = Path(filepath).stem

    with open(filepath, 'rb') as f:
        data = f.read()

    if len(data) < 16 or data[1:4] != b'DLR':
        return None, "not RLD"

    decrypted = decrypt_rld(data, keys)

    # Save decrypted
    if output_dirs.get('dec'):
        dec_path = output_dirs['dec'] / f"{filename}.rld.dec"
        with open(dec_path, 'wb') as f:
            f.write(decrypted)

    # Parse defChara specially
    if is_defchara:
        parse_defchara(decrypted)
        return [], f"parsed {len(CHAR_TABLE)} characters"

    commands = parse_rld(decrypted)
    entries = extract_translatable(commands, filename)

    # Save readable text
    if output_dirs.get('txt') and entries:
        txt_path = output_dirs['txt'] / f"{filename}.txt"
        export_txt(entries, txt_path)

    msg_count = sum(1 for e in entries if e['type'] == 'MESSAGE')
    return entries, f"{len(commands)} cmds, {msg_count} msgs"

#=============================================================================
# Fix Corrupted TSV
#=============================================================================
def fix_corrupted_tsv(rld_dir, working_tsv, output_tsv=None):
    """Fix corrupted ORIGINAL column using RLD as source of truth"""
    if output_tsv is None:
        output_tsv = working_tsv  # Overwrite in place

    rld_dir = Path(rld_dir)

    # Step 1: Generate keys
    print("[*] Generating key tables...")
    keys = generate_key_table(KNOWN_SEEDS['_default'])

    # Step 2: Load defChara first
    defchara_file = rld_dir / 'defChara.rld'
    if defchara_file.exists():
        with open(defchara_file, 'rb') as f:
            data = f.read()
        decrypted = decrypt_rld(data, keys)
        parse_defchara(decrypted)
        print(f"    Loaded {len(CHAR_TABLE)} character IDs")

    # Step 3: Build pristine originals from RLD
    print("[*] Loading pristine originals from RLD files...")
    pristine = {}  # (file, index, type) -> original

    for rld_file in sorted(rld_dir.glob('*.rld')):
        filename = rld_file.stem

        if filename.lower() == 'defchara':
            continue

        with open(rld_file, 'rb') as f:
            data = f.read()

        if len(data) < 16 or data[1:4] != b'DLR':
            continue

        decrypted = decrypt_rld(data, keys)
        commands = parse_rld(decrypted)
        entries = extract_translatable(commands, filename)

        for entry in entries:
            if entry['type'] in ('BRANCH_START', 'MERGE', 'JUMP', 'GOTO_FILE'):
                continue

            idx = entry['index']

            if entry['type'] == 'MESSAGE':
                if entry.get('speaker'):
                    pristine[(filename, idx, 'NAME')] = entry['speaker']
                if entry.get('text'):
                    pristine[(filename, idx, 'TEXT')] = entry['text']
            elif entry['type'] == 'LABEL':
                pristine[(filename, idx, 'LABEL')] = entry['text']
            elif entry['type'].startswith('CHOICE_'):
                pristine[(filename, idx, entry['type'])] = entry['text']

    print(f"    {len(pristine)} entries from RLD")

    # Step 4: Load and fix working TSV
    print(f"[*] Loading working TSV: {working_tsv}")
    lines = []
    fixed_count = 0
    not_found_count = 0

    with open(working_tsv, 'r', encoding='utf-8') as f:
        for line in f:
            # Preserve comments and empty lines
            if line.startswith('#') or '\t' not in line or line.strip() == '':
                lines.append(line)
                continue

            parts = line.rstrip('\r\n').split('\t')
            if len(parts) < 4:
                lines.append(line)
                continue

            file_id = parts[0]
            try:
                index = int(parts[1])
            except ValueError:
                lines.append(line)
                continue

            entry_type = parts[2]
            tsv_original = parts[3].replace('\\n', '\n').replace('\\t', '\t')
            translation = parts[4] if len(parts) >= 5 else ''

            # Lookup pristine
            key = (file_id, index, entry_type)
            if key in pristine:
                pristine_original = pristine[key]
                if tsv_original != pristine_original:
                    # CORRUPTED - fix it
                    fixed_original = pristine_original.replace('\n', '\\n').replace('\t', '\\t')
                    parts[3] = fixed_original

                    # Ensure we have 5 columns
                    while len(parts) < 5:
                        parts.append('')

                    line = '\t'.join(parts) + '\n'
                    fixed_count += 1

                    print(f"    FIXED {file_id}#{index} [{entry_type}]")
                    print(f"      Was: \"{tsv_original[:60]}\"")
                    print(f"      Now: \"{pristine_original[:60]}\"")
            else:
                not_found_count += 1

            lines.append(line)

    # Step 5: Write fixed TSV
    print(f"[*] Writing fixed TSV: {output_tsv}")
    with open(output_tsv, 'w', encoding='utf-8', newline='') as f:
        f.writelines(lines)

    print(f"\n{'='*60}")
    print(f"DONE!")
    print(f"  Fixed: {fixed_count} corrupted entries")
    if fixed_count > 10:
        print(f"         (showed first 10)")
    if not_found_count > 0:
        print(f"  Warning: {not_found_count} TSV entries not found in RLD")
    print(f"{'='*60}")

    return fixed_count

#=============================================================================
# Main
#=============================================================================
def main():
    if len(sys.argv) < 2:
        print("""
╔══════════════════════════════════════════════════════════════╗
║         RLD Tool v3 - Yotsuiro Passionato                    ║
╠══════════════════════════════════════════════════════════════╣
║  • Decrypts RLD files                                        ║
║  • Parses defChara.rld for character names                   ║
║  • Resolves character IDs -> actual names                    ║
║  • Exports translation-ready TSV/JSON                        ║
║  • Fixes corrupted TSV originals                             ║
╚══════════════════════════════════════════════════════════════╝

Usage:
  python rld_tool.py <rld_folder> [output_folder]
      Extract translations from RLD files

  python rld_tool.py --fix <rld_folder> <tsv_file> [output_tsv]
      Fix corrupted ORIGINAL column in TSV using RLD as source

Output:
  output/
    ├── decrypted/          Raw .dec files
    ├── text/               Readable .txt scripts
    ├── translation.tsv     ← Main translation file
    ├── translation.json    Full data for tools
    ├── unique_names.tsv    Speaker names (translate once)
    └── char_table.tsv      Character ID reference
""")
        return

    # --fix mode
    if sys.argv[1] == '--fix':
        if len(sys.argv) < 4:
            print("Usage: python rld_tool.py --fix <rld_folder> <tsv_file> [output_tsv]")
            print("")
            print("  rld_folder  = folder containing .rld files")
            print("  tsv_file    = corrupted translation.tsv to fix")
            print("  output_tsv  = (optional) output file, defaults to overwriting tsv_file")
            return

        rld_dir = sys.argv[2]
        tsv_file = sys.argv[3]
        output_tsv = sys.argv[4] if len(sys.argv) > 4 else None
        fix_corrupted_tsv(rld_dir, tsv_file, output_tsv)
        return

    # Normal extraction mode
    input_dir = Path(sys.argv[1])
    output_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('output')

    output_dirs = {
        'dec': output_dir / 'decrypted',
        'txt': output_dir / 'text',
    }
    for d in output_dirs.values():
        d.mkdir(parents=True, exist_ok=True)

    # Generate key tables
    print("[*] Generating key tables...")
    key_tables = {}
    for name, seed in KNOWN_SEEDS.items():
        key_tables[name] = generate_key_table(seed)
        print(f"    {name}: 0x{seed:08X}")

    # Find all RLD files
    rld_files = sorted(input_dir.glob('*.rld'))
    print(f"\n[*] Found {len(rld_files)} RLD files")

    # Process defChara.rld FIRST
    defchara_file = input_dir / 'defChara.rld'
    if defchara_file.exists():
        print(f"\n[*] Processing defChara.rld first...")
        keys = key_tables.get('_default')
        _, msg = process_file(defchara_file, keys, output_dirs, is_defchara=True)
        print(f"    {msg}")
        for cid, name in list(CHAR_TABLE.items())[:5]:
            print(f"      {cid}: {name}")
        if len(CHAR_TABLE) > 5:
            print(f"      ... and {len(CHAR_TABLE) - 5} more")
    else:
        print("\n[!] defChara.rld not found - character IDs won't be resolved")

    # Process all other files
    print(f"\n[*] Processing script files...\n")
    all_entries = []
    stats = {'ok': 0, 'messages': 0}

    for rld_file in rld_files:
        if rld_file.name.lower() == 'defchara.rld':
            continue  # Already processed

        filename = rld_file.name.lower()
        keys = key_tables.get(filename, key_tables.get('_default'))

        entries, msg = process_file(rld_file, keys, output_dirs)

        if entries is not None:
            print(f"[OK] {rld_file.name} - {msg}")
            all_entries.extend(entries)
            stats['ok'] += 1
            stats['messages'] += sum(1 for e in entries if e['type'] == 'MESSAGE')

    # Export
    print(f"\n[*] Exporting...")

    tsv_path = output_dir / 'translation.tsv'
    tsv_count = export_tsv(all_entries, tsv_path)
    print(f"    {tsv_path} ({tsv_count} entries)")

    json_path = output_dir / 'translation.json'
    export_json(all_entries, json_path)
    print(f"    {json_path}")

    names_path = output_dir / 'unique_names.tsv'
    unique_count = export_unique_names(all_entries, names_path)
    print(f"    {names_path} ({unique_count} unique names)")

    if CHAR_TABLE:
        char_path = output_dir / 'char_table.tsv'
        export_char_table(char_path)
        print(f"    {char_path} ({len(CHAR_TABLE)} characters)")

    # Summary
    print(f"""
{'='*60}
COMPLETE!
  Files processed: {stats['ok']}
  Total messages: {stats['messages']}
  Unique speakers: {unique_count}
  Character IDs: {len(CHAR_TABLE)}

Translation workflow:
  1. Edit unique_names.tsv (global name translations)
  2. Edit translation.tsv (dialogue)
  3. Run: python rld_tool.py --fix rld tl\\translation.tsv
  4. Copy to game folder and run
{'='*60}
""")

if __name__ == '__main__':
    main()