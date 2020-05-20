define dso_local <16 x i16> @src(<16 x i16> %a, <16 x i16> %b) local_unnamed_addr #0 {
  %x1 =  call <16 x i16> @llvm.x86.avx2.psign.w(<16 x i16> %a, <16 x i16> %b) #2
  %y1 =  call <16 x i16> @llvm.x86.avx2.psign.w(<16 x i16> %x1, <16 x i16> %b) #2
  %z1 =  call <16 x i16> @llvm.x86.avx2.psign.w(<16 x i16> %y1, <16 x i16> %b) #2
  ret <16 x i16> %z1
}

declare <16 x i16> @llvm.x86.avx2.psign.w(<16 x i16>, <16 x i16>) nounwind readnone
