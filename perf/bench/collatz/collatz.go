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

func main() {
	var total int64 = 0
	for i := int64(1); i < 5000000; i++ {
		total += steps(i)
	}
	fmt.Println(total)
}
