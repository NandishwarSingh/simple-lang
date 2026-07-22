package main

import "fmt"

func main() {
	var a [800]int64
	var check int64 = 0
	var seed uint64 = 987654321
	for round := 0; round < 600; round++ {
		for i := 0; i < 800; i++ {
			seed = seed*6364136223846793005 + 1442695040888963407
			a[i] = int64(seed >> 40)
		}
		for i := 1; i < 800; i++ {
			v := a[i]
			j := i - 1
			for j >= 0 && a[j] > v {
				a[j+1] = a[j]
				j--
			}
			a[j+1] = v
		}
		check += a[0] + a[799]
	}
	fmt.Println(check)
}
