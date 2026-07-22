package main

import "fmt"

func main() {
	var total int64 = 0
	for i := 0; i < 1000000; i++ {
		s := ""
		for j := 0; j < 8; j++ {
			s = s + "abcdefg"
		}
		total += int64(len(s))
	}
	fmt.Println(total)
}
