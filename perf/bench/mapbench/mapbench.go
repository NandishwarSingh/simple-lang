package main

import (
	"fmt"
	"strconv"
)

func main() {
	a := make(map[int64]int64)
	for i := int64(0); i < 10000000; i++ {
		a[(i*2654435761)%4000037] = i
	}
	var suma int64 = 0
	for i := int64(0); i < 10000000; i++ {
		k := (i * 7919) % 4000037
		if v, ok := a[k]; ok {
			suma += v
		}
	}
	for i := int64(0); i < 1000000; i++ {
		delete(a, (i*31)%4000037)
	}
	suma += int64(len(a)) * 17

	wc := make(map[string]int64)
	for i := int64(0); i < 1500000; i++ {
		w := "w" + strconv.FormatInt((i*131)%9973, 10)
		wc[w]++
	}
	var sumb int64 = 0
	for i := int64(0); i < 9973; i++ {
		w := "w" + strconv.FormatInt(i, 10)
		if v, ok := wc[w]; ok {
			sumb += v * (i + 1)
		}
	}
	var sumc int64 = 0
	for _, v := range wc {
		sumc += v
	}

	fmt.Println(suma + sumb*3 + sumc)
}
