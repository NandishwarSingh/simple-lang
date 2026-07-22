package main

import "fmt"

func popcount(x uint64) int64 {
	var n int64 = 0
	for x != 0 {
		x = x & (x - 1)
		n++
	}
	return n
}

func mix(h uint64) uint64 {
	h = h ^ (h >> 33)
	h = h * 18397679294719823053
	h = h ^ (h >> 29)
	return h
}

func main() {
	var total int64 = 0
	var h uint64 = 12345
	for i := 0; i < 20000000; i++ {
		h = mix(h)
		total += popcount(h)
	}
	fmt.Println(total)
}
