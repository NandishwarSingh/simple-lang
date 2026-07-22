package main

import "fmt"

const N = 8192

func main() {
	var x, y [N]float64
	for i := 0; i < N; i++ {
		x[i] = float64(i*7%1000) / 1000.0
		y[i] = float64(i*13%1000) / 1000.0
	}

	for step := 0; step < 120000; step++ {
		for i := 0; i < N; i++ {
			x[i] = x[i]*0.99 + y[i]*0.01
			y[i] = y[i]*0.99 + x[i]*0.01
		}
	}

	for step := 0; step < 70000; step++ {
		for i := 0; i < N; i++ {
			v := x[i]
			x[i] = ((v*0.5+0.25)*v+0.1)*0.5 + y[i]*0.5
		}
	}

	var a, b [128][128]float64
	for i := 0; i < 128; i++ {
		for j := 0; j < 128; j++ {
			a[i][j] = float64((i*7+j)%100) / 100.0
			b[i][j] = float64((i*5+j*3)%100) / 100.0
		}
	}
	trace := 0.0
	for rep := 0; rep < 200; rep++ {
		var c [128][128]float64
		for i := 0; i < 128; i++ {
			for k := 0; k < 128; k++ {
				aik := a[i][k]
				for j := 0; j < 128; j++ {
					c[i][j] = c[i][j] + aik*b[k][j]
				}
			}
		}
		for i := 0; i < 128; i++ {
			trace = trace + c[i][i]
		}
	}

	s := 0.0
	for i := 0; i < N; i++ {
		s = s + x[i] + y[i]
	}
	s = s + trace
	q := int64(s * 1000000.0)
	fmt.Println(q % 1000000000000)
}
