# Homebrew formula for mac68k-disk. The published copy lives in github.com/bircher988/homebrew-tap
# (Formula/mac68k-disk.rb); update url and sha256 there for each release.
class Mac68kDisk < Formula
  desc "Create and edit MFS and HFS disk images for the classic 68k Macintosh"
  homepage "https://github.com/bircher988/mac68k-disk"
  url "https://github.com/bircher988/mac68k-disk/archive/refs/tags/v1.1.tar.gz"
  sha256 "a090a862a77886df8b55869115f1914a5f62c9b72d248944107eb4fff553ec42"
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
