package main

import "fmt"

const N = 2000000

func main() {
	flags := make([]bool, N)
	for i := range flags {
		flags[i] = true
	}
	var count int64 = 0
	for rep := 0; rep < 10; rep++ {
	count = 0
	for i := range flags { flags[i] = true }
	for i := 2; i < N; i++ {
		if flags[i] {
			count++
			for j := i + i; j < N; j += i {
				flags[j] = false
			}
		}
	}
	}
	fmt.Println(count)
}
