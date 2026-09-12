10 REM Exercise the parts a fresh target tends to get wrong:
11 REM loops, strings, the soft-float maths, arrays and GOSUB.
12 REM Every line prints, and the test compares what came out.
20 LET S = 0
30 FOR I = 1 TO 5
40   LET S = S + I * I
50 NEXT I
60 PRINT "SUMSQ=", S
70 LET A$ = "wikireader"
80 PRINT "STR=", UPPER$(LEFT$(A$, 4)), MID$(A$, 5, 3), LEN(A$)
90 PRINT "MATH=", INT(SQR(144)), ABS(-7), SGN(-3)
100 DIM V(5)
110 FOR I = 1 TO 5
120   LET V(I) = I * 2
130 NEXT I
140 PRINT "ARRAY=", V(1), V(5)
150 GOSUB 300
160 PRINT "FLOAT=", 1 / 4
170 PRINT "BASIC-CHECKS-DONE"
180 END
300 PRINT "GOSUB=ok"
310 RETURN
