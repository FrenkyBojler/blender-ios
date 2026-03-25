/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief Romanization.
 *
 */

#include <string.h>

#include "BLI_romanization.hh" /* Own Include.*/

namespace blender::romanization {

const char mandarin_pinyin_initials[] =
    "ydkqsxhwzssxjbymgcczqpssqbycdscdqldylybsgjgyqzjjfgcclzzbwdwzjljpfyynwjjtmyyzwzhflyppqhgccyyym"
    "jqyxxgjxhsdsjnjjsmhmlzrxyfsngsyczqzggllyjlmyzssecykyyhqwjssggyxyqyjtwkdjhychmyxjtlxjyqbyxdldw"
    "rrjjwysrldzjpcbzjjbrcfslbczstzfxxthtrqggbdlyccscymmrfcyqzpwwjjyfcrwfdfzqpyddwyxkyjawjffxjpdft"
    "zyhhycyswccyqsclcxxwzzxnbgnnxbxlzsqcbsgpysyzdhmdzbqbzcwdzzyytzhbtsyyfzgntnxqywqskbphhlxgybfmj"
    "ebjhhgqtjcysxstkzglyckglysmzxyalmeldccxgzyrcxsdltjzcqkcnnjwhjczzcqljststbnxbtyxceqxgkwjyflzql"
    "yhjqspsfxlfpbyqxxxydcczylllsjxfhjxpjbcffyabyxbhczbjyclwlczggbtssmdtjcxpthyqtgjjscjfzkjzjqnlzw"
    "lslhdzbwjncjzyzsqnycqyrzcjjwybrtwpyftwexcskdzctbxhyzcyyjxzcfbzzmjyxxcdczottbzljwfcgszsxfyrlny"
    "jmbdthjxsqjccsbxyytsyfbjdztgbcnclcyzzbsacyzzscjcshzqydxlbpjllmqxtydzxsqjtzpxlcglqccwjbhctdjjs"
    "fxjejjtlbgxsxjmyjjqpfzasyjncydjxkjcdjszcbartcclnjqmwnqnclllkbybzzsyhqcltwlccrshllzntylnewyzyx"
    "czxxgdkdmtcedejtsyys?dqdfmsd?"
    "jlhrwnqlybglxhlgtgxbqjdzfyjsjyjcjmrnymgrcjczgjmzmgxmmryxkjnymsgmzjymklfxmbdtgfbhcjhkylpfmdxlq"
    "jjsmtqgzsjlqdldgjycalcmzcsdjllnxdjffffjczfmzffpfkhkgdpqxktacjdhhzdddrrcfqyjkqccwjdxhwjlyllzgc"
    "fcqdsmlzpbjjplsbcjggdckkdezsqsckjgcgkdjtjllzycxklqscgjcltfpcqczgwbjdqsdjjbyjhsjddwgfsjgdkccct"
    "llpspkjgqjhzzljplgjgjjthjjyjzcjmlzlyqbgjwmljkxzdznjqsyzmljlljkywxmkjlhskjgbmclyymkxjqlbmclkmd"
    "xxkwyxwslmlpsjqjcqxyjfjtjdxmxxllcrqbsyjbgwywxggbcyxpjtgpepfgdjgbhbnsfjyzjkjkhxqfgqzkfhygkhdgl"
    "lsdjjxpqykybnqsxqnszswhbsxwhxwbzzxdmndjbsbkbbzklylxgwxjjwaqzmywsjqlcjxxjqwjeqxscwetlzhlyyysdz"
    "pyqyzcptlshtzcfycyxyljsdcjjagyslcllyyysglrqqeldxzsccccadycjysfsgbfrsszqsbxjpsjwsdrckgjlgdkzjz"
    "bdktcsyqpyhstcldjlhmxmcgxyzhjdctmhltxzxylymohyjcltyfbqqjbfbdfehtksqhzywwcnxxcdwhhwgyjlegmdqcw"
    "gfjhcsntwydolbygwqwesjpwnmlrydzsztxyqpzgcwxhngpyxshmdqjgztdppbfyhzhhjyfdzwkgkzbldntsxhqeegzxy"
    "lzmmzyjzgszxkhkhtxexxgylyapsthxdwhzydpxagkydxbhnhxkdfjnmyhylpmgocslnzhkxxlbzzlbmlsfbhhgsgyygg"
    "bhscyajtxwlxtzqcwzydqdqmmgdqllszhlsjzwfjhqswscelqazynytlsxthaznkzzsdhlacxtwwcsgqqtddyzbcchyqz"
    "flxpslzygpzsznglydqcbdlxjtctajdkywnsyzljhhdzcwnyyzyomhychhhxhjkzwsxhdnxlyscqydpclyzwmypbkxyjl"
    "kzhtyhaxqsyshxasmchkdscrswjpwqsgzjlwwschs?"
    "hsqnhzsngndaqtbaalzzmsstdqjcjktscjaxplggxhhgoxzcxpdmmhldgtybysjmxhmrcplxjzckzxshflqxccdhxezfc"
    "hzccdytcjyxqhlxdhypjqxnlsyydzozjnyxqezysjyayjkypdghddxsppyzndlthrhxydpcjjhtcxmctlhbynyhmhzllh"
    "nxmylllmdcppxhmxdkycyrdltxjchhznxclcclylnzsxzjzzlnnllwhyqsnjhxynttdkyjpychhyegkcttwlgqrlggtgt"
    "ygyhpyhylqyqgcwyqkpyyyttttlhyhlltyttsplkyzwgywgpydqqzzdqxskcqnmjjzzbxyqmjrtfbbtkhzkbjdjjkdjjt"
    "lbwfzpbtkqtztgpdgntpjyfalqmkgxbcclzfhzclllladpmxdjhlcclgyhdzfgyddgcyyfgydxkssebdhykdkdkhnaxxy"
    "bfbyyhxcqgabfqyjjdmljcsjzllpchbsxgjyndybyqspqwjlzkcddtaccbkzdyzypjzqsjnkktknjdjgyepgtlfyqkasd"
    "ntcyhblgdzhbbydmjrygkzyheyybcmcdtyfzjjhgcjplxhldwxjjkytcyksssmtwcttqzlzbszdtwzxgzagyktywxlhlc"
    "pbclloqmmzsslcmbjcszzkydczxgqjdsmcytzqqlwzqzxssbpkdfqmddzdsddtdmfhtdyzjaqjqkypbdjyyxtljhdrqxx"
    "xhaydhrjlklytwhllrllrcxylbwsrszzsymkzzhhkyhxksmzsyzgcjfbzbsqlfcxxxnxkxwymsddyqwggqmmyhcdzttfg"
    "yyhgstttybykjdhkyjbelhdypjqnfxfdqkzhqkzbyjtzbxhfdxbdaswhawajldyjsfhbldnndnqjtjnchxfjsrfwhzfmd"
    "rfjyhwzpdjkzyjymfcyznynxfbytfwfwygdbnzzzdnytxzemmqbsqehxfzmbmflzzsrsymjgsxwzjsprydjsjgxhjjglj"
    "jynzjjxhgjkymlpeyycsysgqzswhwlyrjlpxslcxmfsmwkcctnxnynpnjszhdzeptxmwywayysywlxjqzqxzdclaeelmc"
    "pjpclwbxsqhfwrtffjtnqjhjqdxhwlbyccfjlalkyyjldxhhycstdywncjtxywdrmdrqhwqcmfjdyzmhmayxjwmyzqsxt"
    "lmrspwwchajbxtgcypxyyrrclmpamgkqjszyjrmyjsnxtplnbappypylxmyzkynldgyjzczhnlmzhhanqmpgwqtzmxxml"
    "lhgdzxyhxkrxycjmffxyhjfsbssqlhxndycannmtcjcyprrnytyqnyymbmsxndlylysljnlqyshqmllyzlzjjjkymzcsf"
    "bzxxmstbjgnxyzhlsnmcqscyznfzlxbrnnnylmnrtgzqysatswryhyjzmzdhzgzdwybsscskxsyhytsxgcqgxzzbhyxjs"
    "crhmkkbsczjyjymkqhzjfnbhmqhysnjnzybknqmcjgqhwlsnzswxkhljhyybqcbfcdsxdldspfzfskjjzwzxsddxjseee"
    "gjscssmgclxxkywyllymwwwgydkzjgggtggsycknjwnjpcxbjjtqtjwdsspjxzxnzxwmelptfsxtllxcljxjjljsxctns"
    "wxlehhlyqrwhsycsqrybyaywjejqfwqcqqcjqgxaldbzzyjgkgxpltqyfxjltpadkyqhpmatlcpdhkxmtxybhblefxdle"
    "egqdymsawhzmljtwyqxlyjzljeeyxbqqffnlyxrdsctgjgxyylkllxqkcctlhjlqmkkzgcyygllljdzgydhzwxpysjbzk"
    "dzgyzzhywyfqytyzszyezklymhjjhtsmqwyzlkyywzcsrkqyqltdxwcdrjklwsqzwbdcqyncjsrszjlkcdcdtlzzzacqq"
    "czddxyplxcbqjylzllljddzjgyjyjzyxnyyynxjxkxdazwyrdljyyyrjlglldrxjcykywnqcclddnyyykyckczhjxcclg"
    "zqjgjwppcqqjysbzzxyjxjbxjfzbsbdsfnsfpzxhdwztdmpptblzzbzdmyypqjrsdzsqzsqxbdgcpzswdwcsqzgmdhzxm"
    "wwfybpdgphtmjthzsmmbgzmbzjcfzhfcbbzmqcfmbcmcjxlgpnjbbxgyhyyjgptzgzmqbqdcgybjxlwzkydpdymgcftpf"
    "xyztzxdzxtgkmtybbclbjaskytssqyymscxfjeglsllszbqjjjaklyldlycctsxmcwfgkkbqxlllljyxtyltyxytdpjhn"
    "hgnkbyqnfjyyzbyyessessgdyhfhwtcjbsdzjtfdmxhcnjzymqwsrxjdzjqpdqbbsdjggfbkjbxdgjhmgwjjjgdllthzh"
    "hyyyyyysxwtyyyccbdbpypzyccztjpzywcbdlfwzcwjdxxhyhlhwczxjtczlcdpxdjczczlyxjjsjbhfxwpywxzptdzzb"
    "dccjhjhmlxbqxxbylrddgjrrctttgqsczwmxfytmwzcwjwxjywcskybzqccttqnhxnkxxkhkfhtswoccjybcmpzzyjbnn"
    "zpbthhjdlscddytyfjpxyngfxbyqxcbhxcbsxtyzdmzysnxsxlhkmzxlthdhkghxjsshqyhhcjyxglhzxcsnhekdtgqxq"
    "ypkdhextykcnymyyypkqyytjxzlthhqtbyqhxbmyhsqckwwyllhcyylnneqxqwmcfbdccmljggxdqktlxkgnqcdgzjwyj"
    "jlyhhqtttnwchhxcxwheszjydjccdbqcdgdnyxzdhcqrxcbmztqcbxwgqwyybxhmbymykdyecmqkyaqyngyzslfykkqgy"
    "ssqyshjgjcnxkzycxsbkyxhyylstycxqthysmgscpmmgcccccmtztasmgqzjhklosqylswtmqsyqkdzljqqyplcycztcq"
    "qpbbqjzclpkhqcyyxxdtdddsjcxffllchqxmjlwcjcxtspycxndtjshjwxdqqjckxyamylsjhmlalykxcyydmamdqmlmc"
    "znnyybzkkyflmchcmlhxrcjjhsylnmtjggzgywjxsrxcwjgjqhqzdqjdzjjzkjkgdzqgjjyjylhzxxcdqhhhestmhlfsb"
    "djsyyshfyssczqlpbdrfrztzdkykgsctgkwdqzrkmsynbcrxqbjyfaxpzzedzcjykbcjwhyjbqdzywnyszptdkzpfpbaz"
    "tklqyhbbzpnbptyzzybhnydcpjmmcycqmcjfzzdcmnlfpbplngqjtbttajzpzbbdnjkljqylnbzqhksjznggqsczkyxch"
    "pzsnbcgzkddzqanzgjkdntlzldwjljzlywtxndjzjhxyatncbgtzcsskmnjpjytsrwxcfjwjjtkhtzplbhsnjzsyjbwbz"
    "yzlstlsbjhdwwqpslmmfbjdwajyzccjtbnnrzwqxcdslqgdsdpdzhjtqqpsqlyyjzlgyhszlctcbjtktyczjtqkbpjlgm"
    "jzdmcsgpynjzjjyyknhrpwszxmtncszzyxybyhyzaxywkcjtllckjjtjhgcxdxyqyczbywblwqcglzgjgqrqcczssbcrb"
    "cskydznljsqgxssjmecnstztpbdlthzwhqwqtzexnqczgweskssbybstscsjccgbfsdqszlccglllzghzcthcnmjgyzaz"
    "nmckcstjmmzckbjygqljyjppldxrgzyxccsnhshgdznlzhzjjcddcbcjflbfqbczzwpqdnhxljcthqwjgylnlszzpcjds"
    "cqqhjqkdxkpbajyemsmjtzdxlcjyryynwjbngzzkmjxltbsllrtpylcsznxjhllhyllqqzqlxymrcwcxsljmczltzldwd"
    "jjllnzggqxppskygyggbfzpdkmwghcxmcgdxjmcjsdycabxjdlnbcddygskydjtxdjjyxmsaqazdzfslqxyjsjzylblxx"
    "wxqqzbjzlfbblylwdsljhxjyzjwtdjcyfqzqzzdcsxzzqlzcdzfchyspympqzmlpplffxjjnzzylsjyyqzfpfzksywjjj"
    "hrdjzzxtxxglghtdxcskyswmmtcwybazbjkshfhgcxmhfqhyxxyzftsjyzbxyxpzlchmzmbxhzzssyfdmncwdabazlxkt"
    "cshhxkxjjzjsthygxsxyyhhhjwxkzxcsbzzwwhhcwtzzzpjxsnxqqjgzyzawllcwxzfxgyxyhxmkyyswsqmnjnaycyspm"
    "jkgwcqhylajjmzxhmmcnzhbhxclxtjpltxyjhdyylttxfszhyxxsjbjyayrsmxyplckdlyhlxrlnllstyzyyqygyhhscc"
    "smcctzcxhyqfpyyrpfflfqtntszllzmhwtcjqyzwtllmlmdwmbzssmzrbpdddlgjjbxccsrzqqygwcsxfwzlxccrbtdzm"
    "cyggdlqsgtjmwljmymmsyhfbjdgyxccpshxczcsbsjwjgjmpbwaffyfnxhydxzylremzgzcyhsszdlljcsqfzxxkptxzg"
    "xjjgbmyyysnbdylbnlhbfzdcyfbmgqrrmsszxysgtznnydzzcdgbjafjbdknzblcsscpsgzycjszlmlrzzbzzldlsllys"
    "xsqzqlyxzlsgkbrxbrbzcycxzjzeeyfgklzlyyhgysgzlfjhgtgwkraajyzkzqtsshjjxdzyz?"
    "yjlzyrzdqqhgjzxsszbtkjpbfrtjxllfqwjgslqtymblpzdxtzagbdhzzrbgjhwnjtjxlhscfsmwlldqysjtxkzscfwjl"
    "bxftzlljzllqblcqmqqcgcdfpbbhzczjlpyygjdtgwdcfczqyyyqysrclqzfklzzzgffsqnwglhjycjjczlqzcyjbjzzb"
    "pdccmhjgxdqdgdlzqmfgpsytsdyfwwdjzjysxyycjcyhzwpbyhxrylybhkjksfxtzjmmchhlltnyymsxxyzpyjjycdyzw"
    "mtjjkqyrhllqxpsgtlwycljscbxjyzfnmlrgjjtyzbsyzmsjyjhgfzqmsyxrszcwtlrtqzsstkxgqggsptgcdnjsgcqcq"
    "hmxggztqydjkzdlbzsxjlhyqgggthqscpyhjhhgnygkggcmjdzllcclxqsftgzslllmlcskctbljzzszmmnytpzsxqhjc"
    "jyqxyexzqzcpshkzzysxcdfgmwqrllqxrfztlysdctmjcsjjdhjnxtnrztzfqrhqgllgcxszsjdjljcytsjtlnyxhszxc"
    "gjzyqpylfhdjsbpcczgjjjqzjqdybssllcmyttmqtbhjqnnygkynqyqmzgcjkpdcgmyzhqllsllclmholzgdylfzsljcq"
    "zlylzcjeshnylljxgjxlyjyyyxnbcljsswcqqcjyllcldjyllzllbnylgqchxyyqoxccqkyjxxhyklksxayqccqkkkkcs"
    "gyxxyqxygwtjohthxpxxcsshcyeychzzcbwqbbwjqcscszsslzylgdesjzmmymcytsdsxxscjpqqsqylyfzychdjdzywc"
    "btjsydjhcyddjlbdjjsodzyqysqkxxdhhgqjyohdyxwgmmmajdybbbppbcmhcpljzsmtxerxjmhqdstpjdcbssmsssthj"
    "tslmmtrcplzszmlqdsdmjmqpnqdxcfynbfsdqqyxhyaykqyddlqyyysszbydslntfgtzqbzmchdhczcwfdxtmqqsphqww"
    "xsrgjcwtjtzzqmgwjjrjhtqjbbgwzfxjhnqfxxqywyyhyscdydhhqmnmdmmcpbszppzzglmzfollcfwhmmsjzttthlmyf"
    "fytzzgzyskjjxqyjzqphmbzzlyghgfmshpcfzsnclpbqsnjszslxjfpmtyjygbxlldlxpzjypjyhhzcywhjylsjexfssz"
    "ywxkzjlladtmlymqjpwxxhxsktqjezrpxxzghmhwqpwqlyjjqjjzszcfhjlchhnxjlqwzjhbmzyxbdhhypylhlhlgfwlc"
    "fyytlhjjcjmscpxstkpnhjxsntyxxtestjctlsslstdlllwwyhdhrjzsfgxssyczykwhtdhwjslhtzdqdjzxxqggyltzp"
    "hcsqfzlnjtclzpfstpdynylgmjllycqhynsbchylhqyqtmzybbywrfqykjsyslzdyjmpxyyssrhzjnyqtqdfzbwwdwwrx"
    "cwhgyhxmkmyyyhmsmzhngcepmlqqmtcwctmhmxjpjjhfxyyzsjzhtybmstsyjdtjjqytlhynbyqzlcxcnzwsmylkfjxlw"
    "gbypjytysylymzckttwlgsmzsylmpwlzwxwqzssaqsyxyrhssntsrapccpwcmgdhhxzdzxfjhgzttsbjhgyglzysmycll"
    "lybtyxhbbzjkssdmalhhycfygmqypjycqxjllljgclzgqlycjcctotyxmtmshllwcgfxymzmklpszzzxhhjyslctyjcyh"
    "xsgyxzkxlzwpyjpdhjwpjpwsqqxlxxdhmrslzcyzwstcxkystzshbsccstplwsscjchjlcgchssphylhfhhxjsxyllnyl"
    "mzdhzxylsxlwzyhcldyahzcmddyspjtqjzlngjfsjshctsdszlblmssmnyymjqbjhrcwtyydchqljapzwbgqybkfcmjwl"
    "zllyylszydwhxpsbcmljpscgbhxlqhyrljxyswxhxzlldfhlslymjljyflyjycdrjlfsyzfsllcqyqfgjyhyszlylmstd"
    "jcyhbzllnwlxxygyyhbmgdhxxhhlzzjzxczzzcyqzfnjwpylcpkpykpmclgkdgxzggwqbdxzzkzfbxxlzxjtpjpttbyts"
    "zzdwslchzhsltjxhqlhyxxxywzyswtmzkhlxzxzpyhgchkjfsyh?"
    "tjrlxfjxptztwhplyxfcrhxshxkjxxyhzjdxjwylhyhmjdbflkhtxcwhcfwjcfpqrxqxcyyyjygrpxgscsxngwchkzdxh"
    "flxxhjjbyzwtsxnncyjjymswzjqrmhxzwfqsylzjzgbhynslbgttcsebhxxwxyhhxyxnsqyxmlywrgyqlxbbcljsylpsy"
    "tjzyhyzawlhorjmksczjxxxyxchcytryxqjddsjfslyltsffyxlmtyjmjjyyyxltzcsxqzlhzxlwyxzhdnlrxhxjcdyhl"
    "brlmbrllaxksllljlyxxlycrylcjcgjcmtlzllcyzzpzpcyawhjjfybdyyzsepckzdqyqpbpcjpdcyzbdbbcyydycnnpj"
    "mtmlrmfmmgwygbsjgygsmdqqqztxmkqwgxllpjgzbqcdjjjfpkjkcxbljmswmdtqjxldlppbxcwkcqqbfqjczagzgmykb"
    "hyyhzykndkzmbpjyspxthlfpnyygxjdbkxnhhjhzjxstrstldxskzysybmxjlxyslbzyslhxjpfxbqnbylljqkygzmcyz"
    "zymccsldlhzgwfwyxzmwcxtynxjhbyymcysbmhysmydyshqyzchmjjmzcaahcbjbbhplxtylsxsdjgjdhkxxtxxnbhnml"
    "ngsltxmrhnlxqjxmzllyswqgdlbjhdcgjyqycmhwfwjybbbyjmjwjmdpwhxqldyapdfxxbcgjspckrssyzjmslbzzjflj"
    "jjlgxzgyxyxlszqyxbexyxhgcxbpldyhwecdwwcjmbtxchxyqxllxflyxlljlssfwdpzsmyjclmswtczbchqekcqbwlcg"
    "ydblqppqzqfjqdjhymmcxtxdrmjwrhxcjzclqxdyynhyyhrslsrsywwzjymtltllgzqcjzyabsckzcjyccqljsqxalmzy"
    "yywlwdxzxqdllqshgpjfjljhjabcqzdjgthhsstcyjlbswzlxzxrwgldlzrlzqtgsllllzlymxqgdzhgbdbhzpbrlw?"
    "xqbpfdwo??whlypcbjcc?dmbzpbzz?"
    "cyqxldomzblzwpdwyygdstthcsqsccrsssyslfybfntyjszdfndpthtzzmbblxlcmyffgtjjqwftmdpjwdnlbzcmmctgb"
    "dzlqlpyfhsymjylsdchdzjwjcctljcldtljjcpddpjdsszynndbjlggjzxsxnlycybjjqxcbylzcfzppgkcxzdzfztjjf"
    "jsjxzbnzyjqttyjwhtyczhymdjxttmpxsflzcdwslshxybzgtfmlcjtacbbmgdewycyzcdszcyhflyctygwhkjyylsjcx"
    "gywjcbhlcsnddbtzbsclyzczzssqdllmqyyhfllqllxfdyhabxggnywyypllsdldllbjcyxjzmlhljdxyyqytdlllbbgb"
    "fdfbbqjzzmdpjhgclgmjjpgaehhbwcqxaxhhhzchxyphjaxhlphjpgpzjqcqzgjjzzgzdmqyybzzphyhybwhazyjhykfg"
    "dpfqsdlzmljxjpgalxzdaglmdgxmwzqytxdxxpfdmmssympfmdmmkxksyzyshdzkjsysmmzzzmsydnzzczxbmlstmddnm"
    "xckjmztyymzmzzmsshhdccjemxxkljstgwlsqlyjzllsjssdbpmhnlyjczyhmxxhgzcjmdhxtkgrmxfwmckmwkdcksxqm"
    "mmfzzydkmsclcmpcgmwrpxqpzdsslcxkyxtmlgjyahzjgzqmcsnxyhmmpmlkjxmhlmlgmxctkzmjjyszjsyszhsyjzjcd"
    "ajzybsdqjzgwzkgxfkdmsdjlfmehkzqkjbeypzyszcdwyjffmzjykttdzzefmzlbnpplplpbpszalltylkckqzkgenqlw"
    "agxxydpxlhsxqqwqykxqclhyxxmlyccwlymqyskychlcjnszkpyzkcqzqljbdmdjhlasqlbydwqlwdnbqcrydddtjybkb"
    "wszdxdtnpjdtctqdfxqqmgnseclstbhpwslctxxlpwydzklzygzcqapllkccylbqmqczqcljslqzdjxldthpzqdljjxzq"
    "djyzhkzljcyqdyjppypeakjyrmpcbymcxkllzllfqpylllmbsglcysslrsysqtmxyxqqzbdzrysyztffmzzsmzqhzsscc"
    "mlyxwtpzgxzjgzgsjsgkddhtqggzllbjdzlcbzhyxyzhzfywxyzymsdbzzyjgtsmtfxqyxjscdgslnmdlrytzlryylxqh"
    "txsrtzcgyxbnqqzfhykmzjbzymkbpnlyzpblmcnqyzzzsjzhjctzhhyzzjrdyzhnfxglfxslkgjtctssyllgzrzbbjzzk"
    "lpkbczyslxyxbjfpnjzzxcdwxzyjxzzdjjgggrsrjkmcmzjlsjywqshyhqjsxpjzzzlsnshrnypjtwchklbsrzlcxwjqx"
    "qkysjycztlqzybbybwzjqdwgyzcytjcjxckcwdkkzxsgkdzxwwyyjqyytcytdjlxwkczkklcclzcqqdzlqlcsfqchqhsf"
    "smqzzllbjjzbsjhtsjdysjqjpdlzcdcwjkjzzlpycgmzwdjjbsjqzsyzyhhxcbbjydssddzncglqmbtsfcbfdzdlznfgf"
    "jgfsmptjqlmblgqcyyxbqkdxjqsrfkztjdhczklbsdzcfytplljgjhtxzcsszzxstcygkgckgyoqxjplzbbbgtgyjdgcz"
    "qszlbjlsjfzgkqqjcgyczbzqtldxrjxbsxxpzxhyzyclwdsjjhxmfczpfzhqhqmqgkslyhtycgfrzgnqxclpdlbzcsczq"
    "lljblhbdcypczppdymtzsgyhckcpzjgslclnscdsldzxbmsdlddfjmkdjdhslzxlszqpqpgjllybdszgqlbzlslkyyhzt"
    "tncjyqtzzfszqztlljtyyllqllqyzqlbdzlslyyzymdfszsnhlxznczqzbbwskrfbcyzmthblgjpmczzcstlxshtzcyzl"
    "zblfeqhlxflcjlyljqcbzlzjghsstbrmhxzhjzclxfnbgxgtqjcztmsfzkjmssnxljkbhszxntnlzdntlmsjxgzjyjczx"
    "yhyhwrwwqnztnfjscpzshzjfyrdjsfscjzbjfzqzchzlxfxsbzqlzsgyftzdcszxzjbqmszkjrhxjzcgbjkhchgtjkjqg"
    "lxbxfgdrtylxjxgdtsjxhjzjjcmzlcqsbtxhqgxttxhxftsdkfjhzyjfjxrzcdlllcqsqqzqwqxswqtwgwbzcgcllqzbc"
    "lmqqtzgzxzxljfrmyzflxysqxxjkxrmjdcdmmyxbsqbhgcmwfwtgmxlzbyytgzyccdxyzxswgxyjyznbgpzjcqsyxcxrt"
    "fycgrhztxszzthcbfclsyxzljqmzlmplmxzjssflbysmyqhxjsxrxsqzzzsslyflczjrcrxhhzxqydshxsjjhzcxjbdyn"
    "sysxjbqlpxzqpymlxzkyxlxcjlcycrxzzlldlllsjyhzxgyjwkjrwyhcpsgnrzlfzwfzznsxgxflzsxzzzbfcsyjdbrjk"
    "rdhhgxjljjtgxjxxstjtjxlyxqfcsgswmsbctlqzzwlzzkxjmltmjyhsddbxgzhdlbmyjfrzfcgclyjbpmlysmsxlszjq"
    "qhjzfxgfqfqbpxzgyyqxgztcqwyltlgwwgwhllfsfgzjmgmgbgtjfsyzzgzyzaflsspmlbflcwbjzcljjmzlpjjlymqdm"
    "yyyfbgygqzglyzdxqyxrqqqhsxyyqqygjtyxfsfsllgnqcygycwfhcccfxbylypllzqxxxxxkqhhxshjdcfdsczjxcpzw"
    "hhhhhapylhalpqafyhxdyllkmzqgggddesrnndltzgchybpysqjjhclljtolnjpzljlhymheydydsqycddhgzpndzclzy"
    "wllznteytgxlhslpjjbdgwxpcdntjcklkclwkllcasstknzdnqnttlyyzssysszzryljqkcgbhhcrxrzydgrgcwcgzhff"
    "fppjfzynakrgywyqpqxxfkjtszzxswzddfbbqtbgtzfznpzfpzxzpjszbmqhkcyxyldkljnypkyghgdcjxxeahpnzgctz"
    "cmxcxmmjxnkszqnmnlwbwwxjjyhclstmcsqdjcxxtpcnpdtnnpglllzcjlspblplkcdtnjnlyyrscffjfqwdpgzdwmnzc"
    "clodaxnssnyzrestyjwjyjdbcfxnmwttbqlwstszgybljpxglboclgpcbjftmxzljylzxcltpnclcgxtfzjshcrxsfysz"
    "dkntlbyjcyjllstgqcbxnwzxbxklylhzlqzlnzcqwgzlgzjncjgcmnzzgjdzxtzjxycyycxxjyyxjjxsssjstssttppgh"
    "tcsxwzdcsyfptfbchfbblzjclzzdbxgcxlqpxkfzflsyltywbmnjhskbmddbcysccldxycddqlyjjhmqllcsgljjsyfpy"
    "yccyltjantjjpwycmmgqyysqdhqmzhszxpftwwzqswqrfkjlxjqqyfbrxjhhfwjgzyqacmyfrhcyybyqwlpexcczstyrl"
    "tsdmqlykmbbgmyyjprknnbbsxyxbhyzdjdnghpmfsgbwfzmfjmmbcmzzcjjlcnyxyqgmlrygqccyhzlwjgcjcggmcjjfy"
    "zzjhycfrrcmtzqzxhfqgdjxccjeaqcrjthpljlszdjrbcqhjdzrhxlyxjsymhzydwldfryhbbydtssccwbxglpzmlzztq"
    "sscpjmmxjcsjytycghycjwsnsxlfemwjnmkllswtxhyyygcmmcwjdqdjzglljwjnkhpzggflccsczmcbltbhbqjxqdjpd"
    "jqtghglfqawbzyjjltstdhqhctcbchflqmpwdshyytqwcnztjtlbypbpdyyyxsqkxwyyflxxncwcxybmaelykkjmzzzbr"
    "xyaqjfljpfhhhytzzxrgqqmhspgdzjwbwpjhzjdyscqwzkthxsqlzyymysdzgrxckkhjlwpysyscsyzlrmlqsyljxbcxt"
    "lhdqzpcycykpppnsxfyzjjrcemhszmsxlxglrwgcstlrsxbygbzgztcpldjlslylymdtmtcpalcxpqjcjwtcyyzlblxbz"
    "lqmyljbghdslssdmxmbdczsxwhamlczcpjmcnhjyjnsygchskqmzzqdllkablwjqsfmocdxjrrlyqchjmybyqlrhetfjz"
    "frfksryxfjdwdsxxlwsqjyslyxwjhsnlxyyxhbhawhhjcxwmyljcsqlkydttxbzsxfdxgxsjhhsxxybssxdpwncmrptjz"
    "czenygcxqfjxkjbdmljcmqqxloxslyxxlylljdzbtymhbfsttqqwlhogyblscalzxqlhtwrrqhlstmypyxjjxmqsjpnbr"
    "yxyjllyqylthylqyfmhkljdmllhfzwkzhljmlhljkljstlqxylmbhhlnlsxqchxcfxxlhyhjjgbyzzkbxscqdjqdsxjzs"
    "yhzhhmgsxcsymxfebcqwwrbpyyjqtyqcyjhqqzyhmwffhgzfrjfcdbxndqyzpcyhhjlfrzgppxzdbbgzqstlgdgylcqmg"
    "chhmfywlzyxkjlypqhsywmqqgqzmlzjnsqxjqsyjtcbehsxfssfxzwfllbcyyjdytdthwzsfjmqqyjlmqsxlldttkhhyb"
    "fpwdyysqqrnqwlgwdebdwcyygcdlkjxtmxmyjsxhybrwfymwfrxyqmxysctzztfykmldhqdlwyqnlcryjblpsxcxywlsb"
    "rrjwxhqybhtydnhhgmmywytzcsqmtssccdalwztcpqpyjllqzyjswxwzzmmglmxclmxczmxmzsqtzppjqblpgxjzhfljj"
    "hycjsnxwcxsccdlxsyjdcqcxslqyclzxlzzxmxqrjmhrhzjphmfljlmlclqnldxzlllfypngjysxcqqdcmqjzzxhnpnxz"
    "mekmxxykyqlxsxtxjxyhwdcwdzhqyybgybcyscfgfsjnzdyzzjzxrzrqjjymcanhrjtldbpyzbstjhxxzypbdwfgzzrpy"
    "mtngxzqbyxmbbfcckrjjjbjegrzgyclkxzdxkknsjkcljspgyyzlqqjybzssqlllkjfcbktylcccdblsppfylgydtzjyq"
    "ggkqttfcxbdkdxxhybbfytyhbclpdytgdhryrnjsbtcsnyjqhklllzslydxxwbcjqsbxbfjzjcjdzfbxxbrmlazgcsncl"
    "bjdstblfrzdswsbxbcllxxlzdjzsjpylyxxyftfffbhjjjgbygjpmmmmsscljmtlyzjxswxtyledqpjmygqzjgdjlqjwj"
    "qllsdgjgygmscljjxdtygjqjqjcjzcjgdzdshqgsjggcjhqxsnjlzzbxhsgzxcxyljxyxyydfqqjhjfxdhctxjyrxysqt"
    "jxyefyyssyxjxncyzxfxcsyszxyyschshxzzzgzzzgfjdldylnpzgyjyzyyqzpbxqbdztzczyxxyhhscxshcggqhjhgxw"
    "sztmzmehyxgebtylzkkwytjzrclekestdbcykqqsayxcjxwwgsbhjszsdhcsjkqcxswxfctynydpzcczjqtzwjqdzzzqz"
    "ljchlsbhpydxpsxshhezdxfptjqyzzxhyaxncfzyyhxgnqmywxtzsjpkhhgymxmxqcxtsbcqsjyxhtyylybcqlmmszmjz"
    "jllcogxzaajzyhjmchhcxzsxzdznleyjjzjbhzwzzsqtzpsxztdsxjjjznyazphhyysrnqdthzhayjyjhdzxzlswclybz"
    "yecwcycrylcxnhzydzydyjdfrjjhtrsqtxyxjrjhojynxelxsfsfjzghpzsxzszdzcqzbyyklsgsjhczshdgqgxyzgxch"
    "xzjwyqwgyhksseqzzndzfkwyssdclzstsymcdhjxxyweyxczaydmpxmdsxybsqmjmzjmtzqlpjyqzcgqhxjhhhxxhlhdl"
    "djqsldwbsxfzzyyschtytyjbhecxhjkgjfxbhyzjfxbwhbdzfyzbcapnpgnydmsxhkhhmamlnbyjtmpxyjmcthjbzyfcg"
    "tyhwphftgzzezsbzegpbmdskftycmhbllhgpzjxzjgzjyxzsbbqsczzlzccstpgxmjsftcczjzdjxcybzlfcjsyzfgszl"
    "ybcwzzbyzdzypswyjgxzbdsysxlgzbzfygczxbzhzftpbgzgejbstgkdmfhyzzjhzllzzgjqzlsfdjsscbzgpdlfzfzsz"
    "yzyzsygcxsntxchczxtzzljfzgqsqyxzjqccccdjcdxzjyqjccgxztdlgscxzsyjjqtcclqdqztqchqqjztezzzpbkkdj"
    "fcjfztybqyqttynlmbdktjcpqzjdzfpjsbnjlgyjdxjdzqkzgqkxclpzjtcjdqbxdjjjstcjnxbxcmslyjcqmtjqwwcjj"
    "njnlllhjcwqtbzqyczczpzzdzyddcyzdzccjgtjfzdprntctjdcqtqndtjnplzbcllctdsxkjzqdpzlbznbtjdcxfczdb"
    "ccjjltqjpldcgzdbbzjcqdcjwynllzlzccdwllxwzlxrsntqjccxkjlsgdfqtddglrlajjtklymkqlldzytdyycygjwyx"
    "dxfrskstcdenqmrkqzhhqkdldazfkypbggpzrebzzykyzspegjjglkqzzzslysywyzwfqznlzzlzhwcgkypqgnpgblplr"
    "rjyxcccgyhsfzfwbzywtgzxyljczwhxzjzblfflgskhyjzeyjhlpllllcygxdrzelrhgklzzyhzlyqszzjzqljzflnbhg"
    "wlczcfjwspyxnlzlxgccpzbllcxbbbbxbbcbbcrnncccyrbbsyldcgqyyqxygmqzwtzydyjhyfwdehzdjywlccntzyjjc"
    "dedpzdztstqjhdymbjnyjzlxtsstphndjxxbyxqtzqddtjtdyztgwscszqflshlglbcjbhdlyzjyckwtydylbnydsdsyc"
    "ctyszyyebgexhqddwnygyclxtdcystqmygzasccszzddlcclzrqxyyeljsbymxshztembbllyyllytdqyshymrqxkfkbf"
    "xnxsbychxbwjyhtqbpbsbwdzylkgzskyghqzjhhxjxgnljkzlyycdxlfwfghljgjybxblybxqpqgztzplncybxdjyqydy"
    "mrbesjyyhkxxstmxrczzywxyhybmcflyzhqyzmqxdbxbzwzmslpdmyckfmzklzcyjycclhxfzlydqzpzygyjyzmzxdzfy"
    "fyttqtchgspczmlccytzxjcytjmkslpzhysnwllytpzctzzcktxdhxxtqcypksmqccyyazhtjpcylzlyjbjxtfnyljyyn"
    "rxcylmmnxjsmybcsysslzylljjgyldzdpqbfzzblfndsqkczfhhhgqmrdsxycstxnqqjpyjbfcxdyqfbnxejdgyqbsrcn"
    "fyyqpghyjdyzxgrhtkyleqdzntsmgklbsgbpyszbytjzsszjcssxzbhbscsbzczptqfzmqflypybbjgszmxxdjmthyskk"
    "bjtxhjcegbsmjyjzcxtmljyxrzzqscxxqptzxmkyxxxjcljprmyygadyskqlsadhrskqxzxztcghztlmlwxybwsycdbhj"
    "hcfcwzsxhytkzlxqshlyczjxemplprcgltbzztlzjcyjgdtclglpllqpjmzpapxyzlaktkdnczzbnzctdqqzjyjgmctxl"
    "tgcszlmlhbglkfwnwzhdxphlfmkydlgxdtwzfrjejctzhydxykshwfzcqshktmqqhtchymjdjskhxzjzbzzxympajqmsd"
    "bxlsklyynwrtsqlscbpdbsgzwyhtlkssswhzzlyytnxjgmjszsxfwnlsoztxgxlsammlbwldszylakqcqctmycfjbslxc"
    "lzjclxxksbzqclhjphqplsxsckslnhpsfqqytxjjzlqldxzjjzdyydjnzptfcdskjfsljhylzqjzlbthydgdjfdbyazxd"
    "zhzjnhhqbyknxjjqczmlljzkspldsclbblxklelxjlbjycxjxgcnlcqplzlznjtzljgyzdzpltqcssfdmnycxgbtjdczn"
    "bgbqyqjwgkfhtnbyqzqgbkpbbyzmtjdytblsqmbsxtbnpdxklemyycjynzdtldykzzxddxhqshdgmzsjycctayrzlpwlt"
    "lkxslzcggexclfxlkjrtlqjaqzncmbqdkkcxglczjzxjhptdjjmzqykqsecqzdshhadmlzfmmzbgntjnnlgbyjbrbtmlb"
    "yjdzxlcjlpldlpcqdhlhzlycblcxzcjadqlmzmmsshmybhbskkbhrsxxjmxsdznzpxlbbragggfchgmsklltsjyycqlcs"
    "kywyehywhbhqywbawykqldqftntkhqcgdqktgpkxhcpdhtwtmssyhbwcrwxhjmkmzngwtmlkfghkjyldyycxwhyeclqhk"
    "qhtdqhhffldxqwgzyydesbpkyrzpjfyyzjceqdzzdlattbbfjllcxdlmjsdxegygsjqxcfbxsszpdyzcxdnyxpfzydlyj"
    "ccpltxlsxyzyrxcyysdylwwndsahjsygyhgywkaxtjzdaxysrltdjssaxfnejdxyzhlxlllzhzsjnyqyqqxyjghzgjcyj"
    "chzlycdshwsgczyjxcllnxzjjyyxnfsmwfpylcyllabwddhwdxjmcxztzpmlqzhsfhzynztlldywlslxhymmylmbwwkyx"
    "yadtxylldjpybpwfxjmmmllhafdllaflbhhhbqqjtzjcqjjdjtffkmmmbythygdcqrddwrqjxnbysnmzdbyytbjhpybyg"
    "tjxaahgqdqtmystqxkbtsbkjlxrbeqqhqmjjbdjwtgtbxpgbktlgqxjjjcdhxqdwjlwrfmqgwqhckryswgbtgygbwsdwd"
    "wrfhwytjjxxxjyzyslphyypayxhydqkxshxyxeskqhywbdddpplcjlhqeewxksyyhdyplfjthkjltcyyhhjttpltzzcdl"
    "thqkcxqysteeywkyzyxxyysddjkllpwmcyhqgxyhcrmbxpllnqydqhxsxxwgdqbshyllpjjjthyjkyphthyyktyezyenm"
    "dshlcrpqfbgfxzbsbtlgxsjbswyysksflxlpplbbblbsfxfyzbsjssylpbbffffsscjdstzsxtryjcyffsytyzbjtbcts"
    "bsdhrtjjbytcxyjeylxcbnebjdsysyhgsjzbxbytfzwgenyhhthjhatfwgcstbgxklstywmtmbyxjskzscdyjrcytwxzf"
    "hmymcxlznsdjtttxrycfyjsbsdyerxhljxbbdeynjghxgckgscymblxjmsznskgxfbnbbthfjaafxyxfpxmyfhdtzcxzz"
    "pxrsywzdlybbjtyqwqjpzypzjznjpzjlztfysbttslmptzrtdxqsjehbzylzdhljsqmlhtxtjecxalzzspktlzkqqyfsy"
    "gywpcpqfhqhytqxzkrsgtgsqczlptxcdyyzssqzslxlzmacbcqbzyxhbsxlzdltcdjtylzjyytpzylltxjsjxhlbmytxc"
    "qrblzssfjzztnjydxmyjhlhpblcyxqjqqkzzscpzkswalqsblcczjsxgwwwygyatjbbctdkhqhkgtgpbkqyslbxbbckbm"
    "llxdzstbklggqkqlsbkkdfxrmdkbftpzfrtbbmferqgxkjpzsstlbzdpszqzsjthljqlzbpmsmmsxlqqnhknblrddnhxd"
    "hddjcyygyfqgzlgsygmjqgkhbpmxyxlytqwlwgcpbmjxcyzydrjbhtdjxeeshtmjsbyplwhlzffnypmhxqhpltbqpfbcw"
    "jdbygpnxtbfzjgsddtjshxeawzzyllttybwjkgxghlfkxdjtmszsqynzggswqsphtlsskmclzxyszqzxncjdqgzdlfnyk"
    "ljcjllzlmzznhydsshthxzlzzbbhqzwwycrdhlyqqjbeyfsgxthsrxwqhwfslmssgzttyeyqqwrslalhmjtqjsmxqbjjz"
    "jxzyzkxbyqxbjxshzssfglxmxzxfghkzszggylclsarjxhslllmzxelglxydjytlfbhbpnlyzfbbhptgjkwetzhkjjxzx"
    "xglljlstgshjjyqlqzfkcgnndjsszfdbctwwseqfhqjbsaqtgypjlbxbmmywxgslzhglzgnyfljbyfdjfrgsfmbyzhqfb"
    "wjsyfyjjphzbyyzffwodgrlmftmlbzgycqxcdjygzyyyytytydwegazyhxjlzythlrmgrjxzclhneljjthtbwjybjjbxj"
    "jtjteekhwsljplpsfazpqqbdlqjjtyyqlyzkdksqjyyjzldqcgjjyzjsycmraqthtejmfctyhypkmhycwjdcfhyyxwshc"
    "txrljgjshccyyyjltkttytmxgtcjtzayyoczlylbszywjytsjyhbyshfjlygjxxtmzyyltxxypclxyjzyzyypnhmymdyy"
    "lblhlsyygqllnjjymsoycbzgdlyxylcqyxtszegxhzglhwbljgeyxtwqmakbpqcgyshhegqcmwyywljyjhyyzlljjylhz"
    "yhmgsljljxcjjyclycjpcpzjzjmmylcjlnqljjjlxxjmlszljqlycmmhcfmmfpqqmfxlqmcffqmmmmhmznfhhjgtthhkh"
    "slnchhyqdxtmmqdcydyxyqmyqylddcyyydazdcymzydlzfffmmycqcwzzmabtbyctdmndzggdftypcgqyttssffwbdtzq"
    "ssystwjjhjytsxxylbyqhwwhxezxwznnqzjzjjqjccchyyxbzxccyjtllcqxknjycyycynzzqyyoewyczdcjycchyjlbt"
    "zkycqwlpgpyllgkdldlgkgqbgychjxy????z?w???z????s?????l???????sl??z????lgtm????gd??????????????"
    "???????atn??hbltdzlatmmbj?lx";

static char mandarin_pinyin_initial(char32_t character)
{
  if (character >= 0x4E00 && character <= 0x9FFF) {
    return mandarin_pinyin_initials[character - 0x4E00];
  }
  return '?';
}

static char cyrillic_initial(char32_t ch)
{
  switch (ch) {
    case 0x0410:
    case 0x0430:
      return 'a'; /* А а */
    case 0x0411:
    case 0x0431:
      return 'b'; /* Б б */
    case 0x0412:
    case 0x0432:
      return 'v'; /* В в */
    case 0x0413:
    case 0x0433:
      return 'g'; /* Г г */
    case 0x0414:
    case 0x0434:
      return 'd'; /* Д д */
    case 0x0415:
    case 0x0435:
      return 'e'; /* Е е */
    case 0x0401:
    case 0x0451:
      return 'y'; /* Ё ё -> "yo" -> 'y' */
    case 0x0416:
    case 0x0436:
      return 'z'; /* Ж ж -> "zh" -> 'z' */
    case 0x0417:
    case 0x0437:
      return 'z'; /* З з */
    case 0x0418:
    case 0x0438:
      return 'i'; /* И и */
    case 0x0419:
    case 0x0439:
      return 'y'; /* Й й -> 'y' */
    case 0x041A:
    case 0x043A:
      return 'k'; /* К к */
    case 0x041B:
    case 0x043B:
      return 'l'; /* Л л */
    case 0x041C:
    case 0x043C:
      return 'm'; /* М м */
    case 0x041D:
    case 0x043D:
      return 'n'; /* Н н */
    case 0x041E:
    case 0x043E:
      return 'o'; /* О о */
    case 0x041F:
    case 0x043F:
      return 'p'; /* П п */
    case 0x0420:
    case 0x0440:
      return 'r'; /* Р р */
    case 0x0421:
    case 0x0441:
      return 's'; /* С с */
    case 0x0422:
    case 0x0442:
      return 't'; /* Т т */
    case 0x0423:
    case 0x0443:
      return 'u'; /* У у */
    case 0x0424:
    case 0x0444:
      return 'f'; /* Ф ф */
    case 0x0425:
    case 0x0445:
      return 'k'; /* Х х -> "kh" -> 'k' */
    case 0x0426:
    case 0x0446:
      return 't'; /* Ц ц -> "ts" -> 't' */
    case 0x0427:
    case 0x0447:
      return 'c'; /* Ч ч -> "ch" -> 'c' */
    case 0x0428:
    case 0x0448:
      return 's'; /* Ш ш -> "sh" -> 's' */
    case 0x0429:
    case 0x0449:
      return 's'; /* Щ щ -> "shch" -> 's' */
    case 0x042A:
    case 0x044A:
      return '?'; /* ъ  hard sign -> no initial */
    case 0x042B:
    case 0x044B:
      return 'y'; /* Ы ы */
    case 0x042C:
    case 0x044C:
      return '?'; /* ь  soft sign -> no initial */
    case 0x042D:
    case 0x044D:
      return 'e'; /* Э э */
    case 0x042E:
    case 0x044E:
      return 'y'; /* Ю ю -> "yu" -> 'y' */
    case 0x042F:
    case 0x044F:
      return 'y'; /* Я я -> "ya" -> 'y' */
    default:
      return '?';
  }
}

char shortcut(char32_t character, const char *language)
{
  if (language == nullptr || language[0] == '\0') {
    return '?';
  }
  if (strcmp(language, "zh_HANS") == 0) {
    return mandarin_pinyin_initial(character);
  }
  if (strcmp(language, "ru") == 0 || strcmp(language, "ru_RU") == 0) {
    return cyrillic_initial(character);
  }
  return '?';
}

/** \} */

}  // namespace blender::romanization
