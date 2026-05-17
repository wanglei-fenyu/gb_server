#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import subprocess
import sys
import json
import shutil
from pathlib import Path

class ThirdPartyUninstaller:
    def __init__(self):
        self.script_dir = Path(__file__).parent.absolute()
        self.root_dir = self.script_dir.parent
        self.packages_file = self.script_dir / "packages.json"
        self.temp_dir = self.script_dir / "temp"
        
    def print_info(self, msg):
        print(f"\033[94m[INFO]\033[0m {msg}")
    
    def print_success(self, msg):
        print(f"\033[92m[SUCCESS]\033[0m {msg}")
    
    def print_error(self, msg):
        print(f"\033[91m[ERROR]\033[0m {msg}")
    
    def print_warning(self, msg):
        print(f"\033[93m[WARNING]\033[0m {msg}")
    
    def run_command(self, cmd, cwd=None):
        try:
            result = subprocess.run(
                cmd, 
                shell=True, 
                cwd=cwd,
                capture_output=True, 
                text=True,
                check=False
            )
            return result.returncode == 0, result.stdout, result.stderr
        except Exception as e:
            return False, "", str(e)
    
    def uninstall_conan_package(self, name, version):
        """卸载 Conan 包"""
        full_name = f"{name}/{version}"
        self.print_info(f"Removing {full_name}...")
        success, stdout, stderr = self.run_command(f"conan remove {full_name} -c")
        if success:
            self.print_success(f"  Removed {full_name}")
            return True
        else:
            self.print_warning(f"  Not found or already removed: {full_name}")
            return False
    
    def clean_temp_dirs(self):
        """清理临时目录"""
        if self.temp_dir.exists():
            shutil.rmtree(self.temp_dir)
            self.print_success("  Cleaned temp directory")
    
    def uninstall_all(self):
        """卸载所有第三方包"""
        self.print_info("Starting third-party packages uninstall...")
        print()
        
        if not self.packages_file.exists():
            self.print_error(f"Packages config not found: {self.packages_file}")
            return False
        
        with open(self.packages_file, 'r', encoding='utf-8') as f:
            config = json.load(f)
        
        packages = config.get('packages', [])
        
        if not packages:
            self.print_warning("No packages found in config")
        
        # 卸载 Conan 包
        self.print_info("Removing Conan packages...")
        for pkg in packages:
            name = pkg.get('name')
            version = pkg.get('version')
            if name and version:
                self.uninstall_conan_package(name, version)
        print()
        
        # 清理临时目录
        self.print_info("Cleaning temporary directories...")
        self.clean_temp_dirs()
        print()
        
        self.print_success("=" * 50)
        self.print_success("Third-party packages uninstalled!")
        self.print_success("Conan is still installed.")
        self.print_success("=" * 50)
        return True
    
    def list_installed(self):
        """列出已安装的包"""
        self.print_info("Installed third-party packages:")
        
        if not self.packages_file.exists():
            self.print_error("Packages config not found")
            return
        
        with open(self.packages_file, 'r', encoding='utf-8') as f:
            config = json.load(f)
        
        found_any = False
        for pkg in config.get('packages', []):
            name = pkg.get('name')
            version = pkg.get('version')
            success, stdout, _ = self.run_command(f"conan list {name}/{version}")
            if success and "Not found" not in stdout:
                self.print_success(f"  {name}/{version}")
                found_any = True
            else:
                self.print_warning(f"  {name}/{version} - NOT INSTALLED")
        
        if not found_any:
            self.print_warning("  No packages found")

def main():
    uninstaller = ThirdPartyUninstaller()
    
    if len(sys.argv) > 1:
        cmd = sys.argv[1].lower()
        if cmd == "list":
            uninstaller.list_installed()
        elif cmd == "help":
            print("Usage:")
            print("  python uninstall.py        - Uninstall third-party packages")
            print("  python uninstall.py list   - List installed packages")
            print("  python uninstall.py help   - Show this help")
        else:
            print(f"Unknown command: {cmd}")
    else:
        print()
        confirm = input("Are you sure? This will remove gbluasocket and gbnet! (y/N): ")
        if confirm.lower() == 'y':
            uninstaller.uninstall_all()
        else:
            print("Cancelled.")

if __name__ == "__main__":
    main()