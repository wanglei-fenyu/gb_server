#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import subprocess
import os
import sys
import json
import shutil
import time
from pathlib import Path

class ThirdPartyInstaller:
    def __init__(self):
        self.script_dir = Path(__file__).parent.absolute()
        self.root_dir = self.script_dir.parent
        self.temp_dir = self.script_dir / "temp"
        self.packages_file = self.script_dir / "packages.json"
        self.profile_path = None   # 可设置的 profile 文件路径
        
    def print_info(self, msg):
        print(f"[INFO] {msg}")
    
    def print_success(self, msg):
        print(f"[SUCCESS] {msg}")
    
    def print_error(self, msg):
        print(f"[ERROR] {msg}")
    
    def print_warning(self, msg):
        print(f"[WARNING] {msg}")
    
    def set_profile(self, profile_path):
        """设置 Conan profile 文件路径，转为绝对路径并检查存在性"""
        path = Path(profile_path).resolve()
        if not path.exists():
            self.print_error(f"Profile file not found: {path}")
            sys.exit(1)
        self.profile_path = path
        self.print_info(f"Using profile: {self.profile_path}")
    
    def run_command(self, cmd, cwd=None):
        try:
            # 设置编码为 UTF-8
            result = subprocess.run(
                cmd, 
                shell=True, 
                cwd=cwd,
                capture_output=True, 
                text=True,
                encoding='utf-8',
                errors='ignore',
                check=False
            )
            return result.returncode == 0, result.stdout, result.stderr
        except Exception as e:
            return False, "", str(e)
    
    def remove_readonly(self, func, path, excinfo):
        """处理只读文件删除"""
        os.chmod(path, 0o777)
        func(path)
    
    def force_rmtree(self, path):
        """强制删除目录"""
        if not path.exists():
            return
        try:
            shutil.rmtree(path, onerror=self.remove_readonly)
        except Exception as e:
            self.print_warning(f"Failed to delete {path}: {e}")
    
    def check_package_installed(self, name, version):
        success, stdout, _ = self.run_command(f"conan list {name}/{version}")
        return success and "Not found" not in stdout
    
    def install_from_git(self, name, version, git_url, tag):
        self.print_info(f"Installing {name}/{version} from {git_url}...")
        
        self.temp_dir.mkdir(exist_ok=True)
        package_dir = self.temp_dir / name
        
        # 强制删除旧目录
        if package_dir.exists():
            self.print_info(f"Removing old directory: {package_dir}")
            self.force_rmtree(package_dir)
            time.sleep(1)
        
        self.print_info(f"Cloning {git_url} (tag: {tag})...")
        success, stdout, stderr = self.run_command(
            f"git clone --branch {tag} --depth 1 {git_url} {package_dir}"
        )
        
        if not success:
            self.print_error(f"Failed to clone {name}: {stderr}")
            return False
        
        self.print_info(f"Building {name}...")
        
        # 验证 profile 是否有效（如果指定了）
        if self.profile_path:
            test_cmd = f'conan profile show --profile="{self.profile_path}"'
            success, stdout, stderr = self.run_command(test_cmd)
            if not success:
                self.print_error(f"Profile {self.profile_path} is invalid: {stderr}")
                return False
            self.print_info("Profile validation passed.")
        
        # 构建 conan create 命令
        cmd = f"conan create . --version={version} --build=missing"
        if self.profile_path:
            # 使用双引号包裹路径，避免空格；subprocess 的 shell=True 会正确处理
            cmd += f' --profile="{self.profile_path}"'
        # 可选：强制重新编译 Boost（如果 profile 中定义了宏）
        # cmd += " --build=boost"
        
        self.print_info(f"Executing: {cmd}")
        success, stdout, stderr = self.run_command(cmd, cwd=package_dir)
        
        # 输出命令的 stdout 和 stderr（前500字符）以便排查
        if stdout:
            self.print_info(f"stdout: {stdout[:500]}")
        if stderr:
            self.print_info(f"stderr: {stderr[:500]}")
        
        # 无论成功与否，都删除临时目录
        self.print_info(f"Cleaning up {package_dir}...")
        self.force_rmtree(package_dir)
        
        if not success:
            self.print_error(f"Failed to build {name}: {stderr}")
            return False
        
        self.print_success(f"{name}/{version} installed successfully")
        return True
    
    def install_all(self):
        self.print_info("Starting third-party packages installation...")
        
        # 检查 Conan 是否可用
        success, _, _ = self.run_command("conan --version")
        if not success:
            self.print_error("Conan is not installed or not in PATH")
            return False
        
        # 加载 packages.json
        if not self.packages_file.exists():
            self.print_error(f"Packages config not found: {self.packages_file}")
            return False
        
        with open(self.packages_file, 'r', encoding='utf-8') as f:
            config = json.load(f)
        
        packages = config.get('packages', [])
        if not packages:
            self.print_warning("No packages found in config")
            return True
        
        installed_count = 0
        
        for pkg in packages:
            name = pkg.get('name')
            version = pkg.get('version')
            git_url = pkg.get('git')
            tag = pkg.get('tag', f'v{version}')
            
            if not name or not version or not git_url:
                self.print_warning(f"Skipping invalid package config: {pkg}")
                continue
            
            # 先强制卸载旧的（避免版本冲突）
            self.print_info(f"Removing old {name}/{version} if exists...")
            self.run_command(f"conan remove {name}/{version} -c")
            time.sleep(1)
            
            if self.install_from_git(name, version, git_url, tag):
                installed_count += 1
            else:
                self.print_error(f"Failed to install {name}/{version}")
                return False
        
        # 清理临时目录
        if self.temp_dir.exists():
            self.force_rmtree(self.temp_dir)
        
        self.print_success(f"Installation completed! Installed: {installed_count}")
        return True
    
    def list_installed(self):
        self.print_info("Installed third-party packages:")
        
        if not self.packages_file.exists():
            self.print_error("Packages config not found")
            return
        
        with open(self.packages_file, 'r', encoding='utf-8') as f:
            config = json.load(f)
        
        for pkg in config.get('packages', []):
            name = pkg.get('name')
            version = pkg.get('version')
            if self.check_package_installed(name, version):
                self.print_success(f"  {name}/{version}")
            else:
                self.print_warning(f"  {name}/{version} - NOT INSTALLED")
    
    def uninstall_all(self):
        self.print_warning("Uninstalling all third-party packages...")
        
        if not self.packages_file.exists():
            self.print_error("Packages config not found")
            return
        
        with open(self.packages_file, 'r', encoding='utf-8') as f:
            config = json.load(f)
        
        for pkg in config.get('packages', []):
            name = pkg.get('name')
            version = pkg.get('version')
            
            self.print_info(f"Removing {name}/{version}...")
            self.run_command(f"conan remove {name}/{version} -c")
            self.print_success(f"  Removed {name}/{version}")
        
        if self.temp_dir.exists():
            self.force_rmtree(self.temp_dir)
        
        self.print_success("Uninstall completed!")

def main():
    installer = ThirdPartyInstaller()
    
    # 解析参数，支持 --profile <file>
    args = sys.argv[1:]
    profile = None
    
    # 手动查找 --profile 及其值
    i = 0
    while i < len(args):
        if args[i] == '--profile':
            if i + 1 < len(args):
                profile = args[i + 1]
                # 移除这两个元素
                args.pop(i)
                args.pop(i)
                continue
            else:
                print("[ERROR] --profile requires a file path argument")
                sys.exit(1)
        i += 1
    
    # 如果提供了 profile，设置它（文件不存在会报错退出）
    if profile:
        installer.set_profile(profile)
    
    # 处理剩余命令
    if len(args) > 0:
        cmd = args[0].lower()
        if cmd == "list":
            installer.list_installed()
        elif cmd == "uninstall":
            installer.uninstall_all()
        elif cmd == "help":
            print("Usage:")
            print("  python setup.py                         - Install all packages")
            print("  python setup.py --profile <file>        - Install using specified profile")
            print("  python setup.py list                    - List installed packages")
            print("  python setup.py uninstall               - Uninstall all packages")
            print("  python setup.py help                    - Show this help")
        else:
            print(f"Unknown command: {cmd}")
    else:
        success = installer.install_all()
        sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()