package xfsmk

import "testing"

func TestLog2Exact(t *testing.T) {
	cases := []struct {
		in   uint64
		want uint8
		ok   bool
	}{
		{1, 0, true}, {2, 1, true}, {512, 9, true}, {4096, 12, true},
		{1 << 40, 40, true},
		{0, 0, false}, {3, 0, false}, {1000, 0, false}, {4097, 0, false},
	}
	for _, c := range cases {
		got, ok := log2exact(c.in)
		if ok != c.ok || (ok && got != c.want) {
			t.Errorf("log2exact(%d) = %d, %v; want %d, %v", c.in, got, ok, c.want, c.ok)
		}
	}
}

// agblklog is how many bits of an inode number are the block, so it must
// cover a group that is not a power of two.
func TestLog2Ceil(t *testing.T) {
	cases := []struct {
		in   uint64
		want uint8
	}{
		{1, 0}, {2, 1}, {3, 2}, {4, 2}, {5, 3}, {32768, 15}, {32769, 16}, {131072, 17},
	}
	for _, c := range cases {
		if got := log2ceil(c.in); got != c.want {
			t.Errorf("log2ceil(%d) = %d, want %d", c.in, got, c.want)
		}
	}
}

func TestRoundUp(t *testing.T) {
	for _, c := range []struct{ n, to, want uint64 }{
		{0, 8, 0}, {1, 8, 8}, {8, 8, 8}, {9, 8, 16}, {4095, 4096, 4096}, {4096, 4096, 4096},
	} {
		if got := roundUp(c.n, c.to); got != c.want {
			t.Errorf("roundUp(%d, %d) = %d, want %d", c.n, c.to, got, c.want)
		}
	}
}

func TestClamp(t *testing.T) {
	for _, c := range []struct{ n, lo, hi, want uint64 }{
		{5, 1, 10, 5}, {0, 1, 10, 1}, {11, 1, 10, 10}, {1, 1, 10, 1}, {10, 1, 10, 10},
	} {
		if got := clamp(c.n, c.lo, c.hi); got != c.want {
			t.Errorf("clamp(%d, %d, %d) = %d, want %d", c.n, c.lo, c.hi, got, c.want)
		}
	}
}

func TestPutLE32(t *testing.T) {
	buf := make([]byte, 4)
	putLE32(buf, 0x01020304)
	if buf[0] != 4 || buf[3] != 1 {
		t.Errorf("putLE32 wrote %x, want little-endian", buf)
	}
}

// The checksum must match what the kernel's reader computes, which the
// live tests confirm against Linux; here it only has to be stable and to
// land in the right place.
func TestSeal(t *testing.T) {
	buf := make([]byte, 512)
	copy(buf, "minikern")
	seal(buf, 224)
	first := make([]byte, 512)
	copy(first, buf)
	// Sealing again over the same bytes gives the same answer.
	seal(buf, 224)
	for i := range buf {
		if buf[i] != first[i] {
			t.Fatalf("seal is not stable at byte %d", i)
		}
	}
	if buf[224] == 0 && buf[225] == 0 && buf[226] == 0 && buf[227] == 0 {
		t.Error("seal wrote no checksum")
	}
	// A change anywhere changes it.
	buf[100] ^= 0xff
	seal(buf, 224)
	same := true
	for i := 224; i < 228; i++ {
		if buf[i] != first[i] {
			same = false
		}
	}
	if same {
		t.Error("the checksum did not change with the contents")
	}
}

func BenchmarkLog2Exact(b *testing.B) {
	for b.Loop() {
		_, _ = log2exact(4096)
	}
}

func BenchmarkLog2Ceil(b *testing.B) {
	for b.Loop() {
		_ = log2ceil(131072)
	}
}

func BenchmarkRoundUp(b *testing.B) {
	for b.Loop() {
		_ = roundUp(4097, 4096)
	}
}

func BenchmarkClamp(b *testing.B) {
	for b.Loop() {
		_ = clamp(5, 1, 10)
	}
}

func BenchmarkSeal(b *testing.B) {
	buf := make([]byte, 4096)
	for b.Loop() {
		seal(buf, 4)
	}
}

func BenchmarkPutLE32(b *testing.B) {
	buf := make([]byte, 4)
	for b.Loop() {
		putLE32(buf, 1)
	}
}
