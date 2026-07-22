package main

import "fmt"

func isPrime(n int64) bool {
	if n < 2 {
		return false
	}
	for d := int64(2); d*d <= n; d++ {
		if n%d == 0 {
			return false
		}
	}
	return true
}

func main() {
	var count int64 = 0
	for n := int64(2); n < 3000000; n++ {
		if isPrime(n) {
			count++
		}
	}
	fmt.Println(count)
}
