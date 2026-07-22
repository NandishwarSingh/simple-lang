package main

import "fmt"

func steps(n int64) int64 {
	var s int64 = 0
	for n != 1 {
		if n%2 == 0 {
			n = n / 2
		} else {
			n = 3*n + 1
		}
		s++
	}
	return s
}

func worker(lo, hi int64, results chan int64) {
	var t int64 = 0
	for i := lo; i < hi; i++ {
		t += steps(i)
	}
	results <- t
}

func main() {
	results := make(chan int64, 8)
	var chunk int64 = 625000
	for w := int64(0); w < 8; w++ {
		lo := w * chunk
		if w == 0 {
			lo = 1
		}
		hi := (w + 1) * chunk
		go worker(lo, hi, results)
	}
	var total int64 = 0
	for w := 0; w < 8; w++ {
		total += <-results
	}
	fmt.Println(total)
}
