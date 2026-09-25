# Homebrew formula for mac68k-disk. Lives in a tap (e.g. <user>/homebrew-tap):
#   brew tap <user>/tap && brew install mac68k-disk
# Update url/sha256 for each release: shasum -a 256 mac68k-disk-<version>.tar.gz
class Mac68kDisk < Formula
  desc "Disk images for the classic 68k Macintosh (MFS and HFS)"
  homepage "https://github.com/bircher988/mac68k-disk"
  url "https://github.com/bircher988/mac68k-disk/archive/refs/tags/v1.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"
  head "https://github.com/bircher988/mac68k-disk.git", branch: "main"

  def install
    system "make", "PREFIX=#{prefix}"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system "#{bin}/mac68k-disk", "version"
    system "#{bin}/mac68k-disk", "new", "test.dsk"
    (testpath/"note.txt").write "Hello"
    system "#{bin}/mac68k-disk", "add", "test.dsk", "note.txt", "--type", "TEXT", "--creator", "ttxt"
    assert_match "note.txt", shell_output("#{bin}/mac68k-disk ls test.dsk")
  end
end
