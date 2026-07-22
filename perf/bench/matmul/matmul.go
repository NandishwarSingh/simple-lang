package main

import "fmt"

var a, b, c [200][200]int64

func main() {
	for i := 0; i < 200; i++ {
		for j := 0; j < 200; j++ {
			a[i][j] = int64((i + j) % 7)
			b[i][j] = int64((i * j) % 5)
			c[i][j] = 0
		}
	}
	for rep := 0; rep < 20; rep++ {
	for i := 0; i < 200; i++ {
		for k := 0; k < 200; k++ {
			aik := a[i][k]
			for j := 0; j < 200; j++ {
				c[i][j] += aik * b[k][j]
			}
		}
	}
	}
	var sum int64 = 0
	for i := 0; i < 200; i++ {
		sum += c[i][i]
	}
	fmt.Println(sum)
}
